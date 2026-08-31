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
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "string-extra.h"
#include "panic.h"
#include "iap-core.h"
#include "iap-artwork.h"
#include "iap-lingo.h"
#include "button.h"
#include "config.h"
#include "cpu.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "serial.h"
#include "appevents.h"
#include "core_alloc.h"

#include "playlist.h"
#include "playback.h"
#include "cuesheet.h"
#include "audio.h"
#include "settings.h"
#include "misc.h"        /* setvol(), to lift an accessory mute on detach */
#include "metadata.h"
#include "sound.h"
#include "action.h"
#include "powermgmt.h"
#include "power.h"        /* charging_state(), the charger IC's own signal */
#include "usb.h"
#ifdef USB_ENABLE_AUDIO
#include "../usbstack/usb_audio.h"
#include "pcm_mixer.h"
#endif

#include "tuner.h"
#if CONFIG_TUNER
#include "ipod_remote_tuner.h"
#endif

/* Transport abstraction — defaults to serial UART, can be overridden for USB HID */
static void iap_serial_tx(const unsigned char *buf, int len)
{
    int i;
    for (i = 0; i < len; i++)
    {
        while(!tx_rdy()) ;
        tx_writec(buf[i]);
    }
}

void (*iap_transport_send)(const unsigned char *buf, int len) = iap_serial_tx;

/* MS_TO_TICKS converts a milisecond time period into the
 * corresponding amount of ticks. If the time period cannot
 * be accurately measured in ticks it will round up.
 */
#if (HZ>1000)
#error "HZ is >1000, please fix MS_TO_TICKS"
#endif
#define MS_PER_HZ (1000/HZ)
#define MS_TO_TICKS(x) (((x)+MS_PER_HZ-1)/MS_PER_HZ)
/* IAP specifies a timeout of 25ms for traffic from a device to the iPod.
 * Depending on HZ this cannot be accurately measured. Find out the next
 * best thing.
 */
#define IAP_PKT_TIMEOUT (MS_TO_TICKS(25))

static bool iap_started = false;
static bool iap_setupflag = false, iap_running = false;
/* This is set to true if a SYS_POWEROFF message is received,
 * signalling impending power off
 */
static bool iap_shutdown = false;

/* iap_periodic()'s 500 ms phase and the mute it last reported. Both
 * were function-statics, so they carried across a detach: a new
 * accessory inherited whatever phase the last one left, and the first
 * tick of its session could be four ticks from the next 500 ms event.
 * In the harness that showed up as a case passing on one target and
 * failing on the other purely because of how many cases ran before it. */
static int periodic_count;
static bool periodic_last_mute;
static unsigned int contents_seed;
static bool contents_shuffled;
static struct timeout iap_task_tmo;
#ifdef USB_ENABLE_AUDIO
static unsigned long iap_audio_reported_frequency;
static unsigned long iap_audio_pending_frequency;
#endif

unsigned long iap_remotebtn = 0;
/* Used to make sure a button press is delivered to the processing
 * backend. While this is !0, no new incoming messasges are processed.
 * Counted down by remote_control_rx()
 */
int iap_repeatbtn = 0;
/* Used to time out button down events in case we miss the button up event
 * from the device somehow.
 * If a device sends a button down event it's required to repeat that event
 * every 30 to 100ms as long as the button is pressed, and send an explicit
 * button up event if the button is released.
 * In case the button up event is lost any down events will time out after
 * ~200ms.
 * iap_periodic() will count down this variable and reset all buttons if
 * it reaches 0
 */
unsigned int iap_timeoutbtn = 0;
bool iap_btnrepeat = false, iap_btnshuffle = false;
/* Stop, Table 4-14 (p.226) byte 0 bit 7. Latched like the two above:
 * the accessory repeats its button status every 30 to 100 ms and a
 * held Stop must stop once. */
bool iap_btnstop = false;
bool iap_btnalbum = false;
bool iap_btnchapter = false;

/* Accessory Power lingo 0x05, Table C-37 (p.548): 0x02 BeginHighPower
 * and 0x03 EndHighPower, both Origin: Apple device.
 *
 * C.8 (p.547): the lingo "is intended for use in conjunction with audio
 * playback from the Apple device. The accessory must remain in low
 * power mode ... until it receives a BeginHighPower command ... The
 * EndHighPower command notifies the accessory that it must stop
 * drawing high power and return to low power mode within 1 second of
 * receiving the command."
 *
 * BeginHighPower went out at identification, twice, with no reference
 * to whether anything was playing, and EndHighPower was never sent at
 * all. So an FM transmitter powered its RF stage the moment it was
 * recognised and held it for the whole attach -- up to 100 mA with the
 * player stopped, out of a battery that is the point of the device.
 * The only escape was NotifyiPodStateChange at shutdown.
 *
 * Table C-38 (p.548) names the version this device advertises, 1.01,
 * for exactly this: "BugFix: BeginTransmission command sent after
 * accessory inserted while the Apple device is playing." */
static bool high_power_wanted;      /* the accessory negotiated 0x05 */
static bool high_power_on;          /* what it was last told */

void iap_high_power_arm(void)
{
    high_power_wanted = true;
    high_power_on = false;
    iap_wake();
}

/* Called from iap_periodic(), in thread context, where this thread owns
 * the TX buffer. */
static void iap_high_power_track(void)
{
    bool want;

    if (!high_power_wanted || !DEVICE_LINGO_SUPPORTED(0x05))
        return;

    want = (iap_play_state_byte() != 0x00);
    if (want == high_power_on)
        return;

    high_power_on = want;
    IAP_TX_INIT(0x05, want ? 0x02 : 0x03);
    IAP_TX_PUT_IPOD_TRANSID();
    iap_send_tx();
}
#if CONFIG_TUNER
/* Play/Pause toggles the radio mute, and MFi 4.2.7 (p.226) has the
 * accessory repeat its button status every 30 to 100 ms for as long as
 * the button is held -- so a one-second hold arrived as 10 to 33
 * packets and toggled the radio once per packet. Debounced the same way
 * Shuffle and Repeat are, by a flag iap_periodic() clears when the
 * button goes up. */
bool iap_btnradiomute = false;
#endif

/* This stack was 6KB and sits immediately below serbuf with no gap, so
 * it grows down straight into the RX buffer. Measured worst cases on the
 * linked image put it over:
 *
 *   iap_handlepkt_mode4 reserves 3328 bytes in its prologue and
 *   iap_handlepkt_mode3 3064, because each puts a struct mp3entry (2760
 *   bytes) and one or two struct playlist_track_info (272 each) on the
 *   stack unconditionally. Add iap_get_trackinfo, get_metadata_ex and a
 *   WAV or Wave64 parse -- parse_list_chunk alone is 1864, the largest
 *   of the 34 metadata parsers -- and a lingo 4 track-title query on a
 *   WAV file reaches about 6.5KB. A plain MP3 current-track query still
 *   reaches roughly 6.2KB once the codepage table is loaded.
 *
 * Only stack[0] is canary-checked, and only at a thread switch, so an
 * overflow that misses word 0 silently corrupts the RX buffer instead
 * of panicking. Any accessory that polls track titles drives this, so
 * it is ordinary traffic rather than an edge case.
 *
 * tagcache's scanning thread gets DEFAULT_STACK_SIZE + 0x4000 for the
 * same get_metadata_ex() workload and runs at 1.84x its measured worst
 * case; x12 puts this thread at a comparable margin.
 */
static long thread_stack[(DEFAULT_STACK_SIZE*12)/sizeof(long)];
static struct event_queue iap_queue;

/* These are pointer used to manage a dynamically allocated buffer which
 * will hold both the RX and TX side of things.
 *
 * iap_buffer_handle is the handle returned from core_alloc()
 * iap_buffers points to the start of the complete buffer
 *
 * The buffer is partitioned as follows:
 * - TX_BUFLEN+6 bytes for the TX buffer
 *   The 6 extra bytes are for the sync byte, the SOP byte, the length indicators
 *   (3 bytes) and the checksum byte.
 *   iap_txstart points to the beginning of the TX buffer
 *   iap_txpayload points to the beginning of the payload portion of the TX buffer
 *   iap_txnext points to the position where the next byte will be placed
 *
 * - RX_BUFLEN+2 bytes for the RX buffer
 *   The RX buffer can hold multiple packets at once, up to it's
 *   maximum capacity. Every packet consists of a two byte length
 *   indicator followed by the actual payload. The length indicator
 *   is two bytes for every length, even for packets with a length <256
 *   bytes.
 *
 *   Once a packet has been processed from the RX buffer the rest
 *   of the buffer (and the pointers below) are shifted to the front
 *   so that the next packet again starts at the beginning of the
 *   buffer. This happens with interrupts disabled, to prevent
 *   writing into the buffer during the move.
 *
 *   iap_rxstart points to the beginning of the RX buffer
 *   iap_rxpayload starts to the beginning of the currently recieved
 *   packet
 *   iap_rxnext points to the position where the next incoming byte
 *   will be placed
 *   iap_rxlen is not a pointer, but an indicator of the free
 *   space left in the RX buffer.
 *
 * The RX buffer is placed behind the TX buffer so that an eventual TX
 * buffer overflow has some place to spill into where it will not cause
 * immediate damage. See the comments for IAP_TX_* and iap_send_tx()
 */
#define IAP_MALLOC_SIZE (TX_BUFLEN+6+RX_BUFLEN+2)
#ifdef IAP_MALLOC_DYNAMIC
static int iap_buffer_handle;
#endif
static unsigned char* iap_buffers;
static unsigned char* iap_rxstart;
static unsigned char* iap_rxpayload;
static unsigned char* iap_rxnext;
static uint32_t iap_rxlen;
static unsigned char* iap_txstart;
unsigned char* iap_txpayload;
unsigned char* iap_txnext;

/* The versions of the various Lingoes we support. A major version
 * of 0 means unsupported
 */
unsigned char lingo_versions[32][2] = {
    {1, 9},     /* General lingo, 0x00 */
#ifdef HAVE_LINE_REC
    {1, 1},     /* Microphone lingo, 0x01 */
#else
    {0, 0},     /* Microphone lingo, 0x01, disabled */
#endif
    {1, 2},     /* Simple remote lingo, 0x02 */
    {1, 5},     /* Display remote lingo, 0x03 */
    {1, 12},    /* Extended Interface lingo, 0x04 */
    {1, 1},     /* RF/BT Transmitter lingo, 0x05 */
    {0, 0},     /* USB Host lingo, 0x06, disabled */
#if CONFIG_TUNER
    {1, 0},     /* RF Receiver lingo, 0x07 */
#else
    {0, 0},     /* RF Receiver lingo, 0x07 disabled */
#endif
    {0, 0},     /* Accessory Equalizer lingo, 0x08, disabled */
    {0, 0},     /* Reserved, 0x09 */
#ifdef USB_ENABLE_AUDIO
    {1, 2},     /* Digital Audio lingo, 0x0A */
#else
    {0, 0},     /* Digital Audio lingo, 0x0A, disabled */
#endif
    {}          /* every other lingo, disabled */
};

/* states of the iap de-framing state machine */
enum fsm_state {
    ST_SYNC = 0,    /* wait for 0xFF sync byte */
    ST_SOF,     /* wait for 0x55 start-of-frame byte */
    ST_LEN,     /* receive length byte (small packet) */
    ST_LENH,    /* receive length high byte (large packet) */
    ST_LENL,    /* receive length low byte (large packet) */
    ST_DATA,    /* receive data */
    ST_CHECK    /* verify checksum */
};

static struct state_t {
    enum fsm_state state;   /* current fsm state */
    unsigned int len;       /* payload data length */
    unsigned int check;     /* running checksum over [len,payload,check] */
    unsigned int count;     /* playload bytes counter */
} frame_state = {
    .state = ST_SYNC
};

enum interface_state interface_state = IST_STANDARD;
static volatile bool interface_pause_pending;

struct device_t device;

static void iap_apply_pending_interface_pause(void)
{
    if (!interface_pause_pending)
        return;

    interface_pause_pending = false;
    if ((audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
        == AUDIO_STATUS_PLAY)
        audio_pause();
}

#ifdef IAP_MALLOC_DYNAMIC
static int iap_move_callback(int handle, void* current, void* new);

static struct buflib_callbacks iap_buflib_callbacks = {
    iap_move_callback,
    NULL
};
#endif

void iap_malloc(void);

static void iap_reset_buffers(void)
{
    iap_txstart = iap_buffers;
    iap_txpayload = iap_txstart+5;
    iap_txnext = iap_txpayload;
    iap_rxstart = iap_buffers+(TX_BUFLEN+6);
    iap_rxpayload = iap_rxstart;
    iap_rxnext = iap_rxpayload;
    iap_rxlen = RX_BUFLEN+2;
}

void put_u16(unsigned char *buf, const uint16_t data)
{
    buf[0] = (data >>  8) & 0xFF;
    buf[1] = (data >>  0) & 0xFF;
}

void put_u32(unsigned char *buf, const uint32_t data)
{
    buf[0] = (data >> 24) & 0xFF;
    buf[1] = (data >> 16) & 0xFF;
    buf[2] = (data >>  8) & 0xFF;
    buf[3] = (data >>  0) & 0xFF;
}

uint32_t get_u32(const unsigned char *buf)
{
    /* buf[0] promotes to int, so shifting a value of 128 or more left by
     * 24 overflows a signed 32-bit int, which is undefined. Reached by
     * any field whose top byte has the high bit set: the IDPS lingo
     * bitmask, the notification masks, track indices, capabilities.
     * Widen before shifting. */
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] <<  8) |  (uint32_t)buf[3];
}

uint16_t get_u16(const unsigned char *buf)
{
    return (buf[0] << 8) | buf[1];
}

#if defined(LOGF_ENABLE) && defined(ROCKBOX_HAS_LOGF)
/* Convert a buffer into a printable string, perl style
 * buf contains the data to be converted, len is the length
 * of the buffer.
 *
 * This will convert at most 1024 bytes from buf
 */
static char* hexstring(const unsigned char *buf, unsigned int len) {
    static char hexbuf[4097];
    unsigned int l;
    const unsigned char* p;
    unsigned char* out;
    unsigned char h[] = {'0', '1', '2', '3', '4', '5', '6', '7',
                         '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

    if (len > 1024) {
        l = 1024;
    } else {
        l = len;
    }
    p = buf;
    out = hexbuf;

    /* while, not do/while. On len == 0 the body ran once and then
     * "--l" underflowed an unsigned int to 0xFFFFFFFF, so it walked
     * 4 GB writing two bytes per input byte into a 4 KB buffer. Only
     * reachable with LOGF_ENABLE, which is off -- so it was waiting for
     * whoever next turns logging on to debug the framer. */
    while (l--) {
            *out++ = h[(*p)>>4];
            *out++ = h[*p & 0x0F];
            p++;
    }

    *out = 0x00;

    return hexbuf;
}
#endif

static size_t iap_utf8_complete_prefix(const unsigned char *str, size_t len)
{
    if (len == 0)
        return 0;

    size_t lead = len - 1;
    while (lead > 0 && (str[lead] & 0xc0) == 0x80)
        lead--;

    size_t width = 1;
    if ((str[lead] & 0xe0) == 0xc0)
        width = 2;
    else if ((str[lead] & 0xf0) == 0xe0)
        width = 3;
    else if ((str[lead] & 0xf8) == 0xf0)
        width = 4;

    return width > len - lead ? lead : len;
}


void iap_tx_strlcpy(const char *str)
{
    ptrdiff_t used;
    ptrdiff_t txfree;
    size_t capacity;
    size_t r;

    /* Treat missing metadata strings as empty. */
    if (str == NULL)
        str = "";

    /* Truncate strings to the local and accessory payload limits. */
    used = iap_txnext - iap_txpayload;
    txfree = TX_BUFLEN - used;

    if (device.acc_max_payload) {
        ptrdiff_t accfree = (ptrdiff_t)device.acc_max_payload - used;

        if (txfree > accfree)
            txfree = accfree;
    }

    if (txfree <= 0)
        return;

    capacity = (size_t)txfree;
    r = strlcpy((char *)iap_txnext, str, capacity);

    if (r < capacity)
    {
        /* No truncation occured
         * Account for the terminating \0
         */
        iap_txnext += (r+1);
    } else {
        size_t copied = iap_utf8_complete_prefix(iap_txnext, capacity - 1);

        iap_txnext[copied] = '\0';
        iap_txnext += copied + 1;
    }
}

void iap_reset_auth(struct auth_t* auth)
{
    auth->state = AUST_NONE;
    auth->max_section = 0;
    auth->next_section = 0;

    /* The authentication version belongs to the accessory that declared
     * it, not to the iPod. iap-core.c:1915 sizes the challenge from it
     * -- "(device.auth.version == 0x100) ? 16 : 20" -- so a 2.0
     * accessory that went away left every later 1.0 accessory being
     * sent a 20-byte challenge for a 16-byte signature. Nothing else
     * clears it, and iap-lingo0.c only writes it when a
     * RetDevAuthenticationInfo arrives, which is after the point the
     * stale value has already been used.
     *
     * 0x100 rather than 0, so the value is a version at every instant:
     * 2.4.2 (p.104) makes 1.0 the baseline every accessory supports,
     * and it is the conservative challenge length of the two. */
    auth->version = 0x100;
    auth->deadline = 0;

    /* Echo state from the accessory's last packet. Kept from one
     * session into the next, the first reply to a new accessory quoted
     * an ID it had never sent. */
    auth->tid_hi = 0;
    auth->tid_lo = 0;
    auth->ipod_tid = 0;
    auth->ipod_tid_valid = false;
    /* Transaction IDs are a property of the session, not of the iPod.
     * Left set, this leaks into every later accessory: notifications
     * stay disabled (iap_periodic()), and a legacy Display Remote's
     * 2-byte packets are rejected forever because lingo 3 keeps
     * expecting a transID that is not there. EndIDPS re-asserts it
     * straight after calling iap_reset_device(), so clearing it here
     * does not disturb the IDPS path itself.
     */
    auth->idps = false;
    auth->idps_started = false;
}

static void iap_rx_discard_all(void);

void iap_reset_state(IF_IAP_MP_NONVOID(int port))
{
    if (!iap_running)
        return;

    /* 0 is dock, 1 is headphone.  This is for
       when we eventually maintain independent state */
    IF_IAP_MP((void)port);

    iap_reset_device(&device);
    iap_bitrate_set(global_settings.serial_bitrate);

    /* And drop whatever the accessory left in the buffer. A packet that
     * had arrived whole before the detach was still there afterwards,
     * and iap_handlepkt() drained it into the fresh session: one
     * buffered Identify put the lingoes, the authentication state,
     * radio_present and the power-notify flag all back. MFi 4.3.11
     * (p.255) is explicit that "on accessory detach, event notification
     * is reset to the default disabled state", and this undid that
     * along with everything else the reset had just done.
     *
     * The block that used to be here was #if 0 "XXX this is still
     * screwed up" and reset the framer and the buffers by hand.
     * interface_state is iap_reset_device()'s job now, and the rest is
     * one IRQ-safe call. */
    iap_rx_discard_all();
}

/* What GetRemoteEventStatus last reported, kept apart from the
 * notification path's copies -- see iap_poll_changed_events(). */
static struct {
    uint32_t      track_index;
    unsigned char play_status;
    unsigned char volume;
    unsigned char abs_volume;
    bool          mute;
    unsigned char shuffle;
    unsigned char repeat;
    uint32_t      trackpos_ms;
    uint16_t      trackpos_s;
    uint32_t      chapter_index;
    unsigned char power_state;
    unsigned char battery_level;
    unsigned char hold;
    uint32_t      numtracks;
} polled;

/* The baseline is the session, not the first poll. MFi 4.3.13 (p.263)
 * has the answer cover what "changed since the last
 * GetRemoteEventStatus command"; before there has been one, that can
 * only mean since the accessory arrived. Priming on the first poll
 * instead made the first answer always empty, so an accessory that
 * connected, waited, and then asked was told nothing had happened. */
static void iap_poll_sample(bool report);

static void iap_poll_baseline(void)
{
    /* Sample without reporting. This used to be a second copy of the
     * list in iap_poll_sample(), and adding a field to one and not the
     * other made the first poll of an untouched session report a change
     * that had not happened. One list, two callers. */
    iap_poll_sample(false);
}

static bool audio_attrs_unacked;
static int  audio_attrs_retries;
#define AUDIO_ATTRS_MAX_RETRIES 8

void iap_reset_device(struct device_t* device)
{
    bool leaving_extended;
    int state_irq;

    iap_artwork_reset();
    iap_reset_auth(&(device->auth));
    device->lingoes = 0;
    device->notifications = 0;
    device->changed_notifications = 0;
    device->do_notify = false;
    device->pb_notifications = 0;
    device->do_power_notify = false;
    device->accinfo = ACCST_NONE;
    device->capabilities = 0;
    device->capabilities_queried = 0;
    device->audio_init_pending = false;
    device->audio_attrs_pending = false;
    device->tuner_caps_pending = false;
    device->volume_reported = false;
    /* power_state and battery_level are deliberately left alone. What
     * matters is that the next reading is reported, and power_reported
     * says so on its own -- zeroing the two values as well changes no
     * observable behaviour, as a mutation confirms. */
    device->power_reported = false;
    device->contents_dirty = false;
    device->pb_track_changed = false;
    device->pb_seeking = 0;
    device->pb_ext_state = 0;

    /* The change-detection shadows. Every one of these is "what this
     * accessory was last told", so every one of them belongs to the
     * session -- and a shadow that outlives it makes the first report
     * to the next accessory a comparison against a stranger's value:
     * either suppressed because it happens to match, or sent with the
     * wrong idea of what changed. pb_trackpos_ms and the Display
     * Remote set were cleared and these five were not, which is the
     * kind of gap only a whole-struct comparison finds. */
    device->pb_trackpos_ms = 0;
    device->pb_trackpos_s = 0;
    device->pb_chapter_index = UINT32_MAX;
    device->pb_chapterpos_ms = 0;
    device->pb_chapterpos_s = 0;
    device->pb_numtracks = 0;
    device->numtracks = 0;
    device->pb_play_status = 0;

    /* And the Display Remote half of the same set. Only some of these
     * were cleared, and which ones looked arbitrary because it was:
     * they were added one notification at a time. */
    device->trackpos_ms = 0;
    device->trackpos_s = 0;
    device->track_index = 0;
    device->chapter_index = UINT32_MAX;
    device->chapter_track_index = UINT32_MAX;
    device->chapter_count = 0;
    device->play_status = 0;
    device->volume = 0;
    device->shuffle = 0;
    device->repeat = 0;
    device->equalizer_index = 0;
    device->backlight = 0;
    device->soundcheck = 0;
    device->audiobook = 0;
    device->hold = false;
    device->alarm_state = 0;
    device->alarm_hour = 0;
    device->alarm_minute = 0;
    memset(&device->datetime, 0, sizeof(device->datetime));
    /* ipod_trans_id is not here: this function sets it to 1 further
     * down, which is what MFi 2.6.1.1 (p.111) asks for -- the device
     * restarts its counter at 1 every time it is connected. A zero
     * here would be overwritten by that line anyway. */

    /* A button the departed accessory was holding. PlayControl(StartFF)
     * sets iap_timeoutbtn to IAP_BTN_HELD, about ten seconds at the
     * 10 Hz countdown, so an accessory unplugged mid-seek left
     * BUTTON_RC_RIGHT asserted for that long --
     * button-clickwheel.c ORs remote_control_rx() into every physical
     * press, so the user's clickwheel was unusable and the WPS kept
     * repeating SEEKFWD. A replacement plugged in inside the window
     * inherited the held seek.
     *
     * MFi 4.2.4 (p.218): "if a command has not been received within
     * approximately 200 ms after the last button status command, the
     * button status will be reset to all buttons up." A detach is at
     * least that.
     *
     * repeatbtn goes to 0, not 2. A review suggested 2, so the
     * release edge reaches the 100 Hz tick the way iap-lingo2.c
     * arranges it when it sees all buttons up -- but that is a
     * mid-session transition with an accessory still attached. Here
     * there is nobody to deliver an edge to, and a non-zero
     * iap_repeatbtn makes iap_handlepkt() re-queue the next packet
     * instead of handling it: every command after a reset was
     * deferred, and 775 checks failed at once. */
    iap_remotebtn = BUTTON_NONE;
    iap_timeoutbtn = 0;
    iap_repeatbtn = 0;
    /* The once-per-press latches. iap_periodic() clears these when the
     * auto-release timer lapses, which is under a second away -- but
     * the reset above sets iap_timeoutbtn to 0, so the next tick clears
     * them anyway and the window is only as long as the gap to it.
     * Short is not none: a replug inside that window let the new
     * accessory's first Shuffle, Repeat, Stop or Mute be swallowed as a
     * repeat of the old one's. They are session state and belong here
     * with the rest of it. */
    iap_btnshuffle = false;
    iap_btnrepeat = false;
    iap_btnstop = false;
    /* The accessory that asked for high power has gone, and the next
     * one has to ask for itself. No EndHighPower on the way out: there
     * is nothing left to send it to. */
    high_power_wanted = false;
    high_power_on = false;
    iap_btnalbum = false;
    iap_btnchapter = false;
#if CONFIG_TUNER
    iap_btnradiomute = false;
#endif
    periodic_count = 0;
    /* Equivalent mutant: the notification this shadows is gated on
     * "!device.volume_reported || ...", and volume_reported is cleared
     * a few lines up, so the first report to a new accessory goes out
     * whatever this holds. Kept because the pair has to be reset
     * together for the second report to be right, and because leaving
     * one of a pair behind is what this whole block exists to stop. */
    periodic_last_mute = false;
#ifdef USB_ENABLE_AUDIO
    iap_audio_reported_frequency = 0;
    iap_audio_pending_frequency = 0;
#endif
    audio_attrs_unacked = false;
    audio_attrs_retries = 0;
    /* Mute is session state and belongs here. SetRemoteEventNotification
     * used to clear it as a side effect, which is why nothing noticed it
     * had no other home -- and clearing it there made the flag disagree
     * with a codec still holding the attenuation. */
    /* An accessory that muted us does not get to leave us silent.
     *
     * SetiPodStateInfo mute drives the hardware down with
     * sound_set_volume(sound_min(...)) while keeping the user's level
     * in global_status.volume, and unmuting calls setvol() to put it
     * back. Clearing the flag here was the whole of the detach path, so
     * unplugging a muted accessory left the codec at minimum, the UI
     * showing the old dB and device.mute reporting not-muted -- a
     * player that had gone silent with nothing to say why and no way
     * back except moving the volume by hand.
     *
     * Flagged rather than lifted: setvol() reaches the codec, and this
     * runs from a tick on the 6G. */
    if (device->mute)
        device->unmute_pending = true;
    device->mute = false;

    /* Detach defers the required pause because this can run in interrupt
     * context. */
    state_irq = disable_irq_save();
    leaving_extended = interface_state == IST_EXTENDED;
    if (leaving_extended)
        interface_pause_pending = true;
    interface_state = IST_STANDARD;
    restore_irq(state_irq);
    if (leaving_extended) {
        button_queue_post(SYS_IAP_UI_EXIT, 0);
        iap_wake();
    }
    /* The poll's own copies go with the session, taking the state the
     * new accessory arrived to as its baseline. */
    /* Flagged, not taken.
     *
     * iap_reset_device() runs from a tick on the 6G: serial_acc_tick()
     * is a tick_add_task() and calls iap_reset_state() when the dock
     * accessory goes. That is ARM IRQ mode, and iap_poll_baseline()
     * samples iap_get_trackpos(), which is audio_current_track(), which
     * takes id3_mutex -- and mutex_lock()'s first statement is
     * ASSERT_CPU_MODE(CPU_MODE_THREAD_CONTEXT), an unconditional
     * panicf. So every dock detach panicked the device.
     *
     * The baseline was plain reads until the poll was widened to cover
     * the events it had been missing; that commit put a blocking call
     * behind a function three levels up that is called from an
     * interrupt. Both callers below are on the iAP thread, so taking it
     * there is safe, and the first of them runs within one 10 Hz tick
     * of the reset. */
    device->poll_baseline_pending = true;
    /* Belongs to the accessory that declared it; 0 means "not declared",
     * which iap_tx_strlcpy() reads as the 1024-byte default of p.150 --
     * above TX_BUFLEN, so the local buffer stays the binding limit. */
    device->acc_max_payload = 0;
    device->idps_lingoes = 0;
    device->idps_options = 0;
    device->idps_deviceid = 0;
    device->ipod_trans_id = 1;
    /* Everything an accessory was allowed to change, handed back.
     * Armed here and done from iap_periodic(), because putting the
     * playlist back is not safe from the tick this can run on. */
    iap_arm_settings_restore();

#ifdef HAVE_LINE_REC
    iap_reset_lingo1();
#endif
    iap_reset_lingo2();
    iap_reset_lingo4();
#if CONFIG_TUNER
    /* Set from both identify paths when an accessory declares the RF
     * Tuner lingo, and cleared nowhere a detach reaches -- the only
     * clear is in ipod_remote_tuner.c, and only if something happens to
     * call tuner_get(RADIO_PRESENT) while the line reads disconnected.
     *
     * So an Apple Radio Remote left it set, and a plain Simple Remote
     * plugged in after it had its Play press answered with lingo 0x03
     * and lingo 0x07 traffic for a tuner it had negotiated neither of.
     * MFi 4.3.11 (p.255): "On accessory detach, event notification is
     * reset to the default disabled state."
     *
     * This one lives outside apps/iap, which is why the sweep of
     * per-session statics in 72a63d89ae did not reach it. */
    radio_present = 0;
#endif
}

static int iap_task(struct timeout *tmo)
{
    (void) tmo;

    /* No accessory connected yet -- tick slowly to avoid unnecessary
     * wakeups while the IAP thread is idle. */
    if (!iap_running)
        return MS_TO_TICKS(1000);

    queue_post(&iap_queue, IAP_EV_TICK, 0);

    /* Reduce the tick from 10 Hz to 1 Hz when no active work remains,
     * to save power during idle MFi DAC connections. 100 ms is still
     * needed during the auth handshake, accessory info polling, button
     * repeat, notification delivery and shutdown notification.
     *
     * !DEVICE_AUTH_RUNNING, not "state == AUST_AUTH": the states this
     * has to stay fast for are the handshake, and AUST_NONE is not one
     * of them -- it is the state a detached dock leaves behind.
     * iap_running stays true from the first sync byte for the life of
     * the boot, so requiring AUST_AUTH pinned the tick at 10 Hz from
     * the moment any accessory was unplugged until another one
     * finished authenticating, which is the whole of the time there is
     * nothing to do. */
    if (!DEVICE_AUTH_RUNNING
        && device.accinfo != ACCST_INIT
        && device.accinfo != ACCST_SENT
        && device.accinfo != ACCST_DATA
        && !device.audio_init_pending
        && !device.do_notify
        && device.pb_notifications == 0
        && !iap_shutdown
        && iap_timeoutbtn == 0
        )
        return MS_TO_TICKS(1000);

    return MS_TO_TICKS(100);
}


/* Convert between the codec's volume range and the 0..255 UI volume the
 * accessory protocol uses.
 *
 * MFi R46 Table 4-61 (p.258), event 0x04: the UI volume level is "a value
 * between 0 and 255, with 0 indicating minimum volume and 255 indicating
 * maximum volume", so the scale has to span the whole range the device
 * actually offers.
 *
 * That range differs per target and is taken from the codec at runtime:
 * -60..+12 dB on the iPod Classic's CS42L55, -90..+6 dB on the iPod
 * Video's WM8758. The old constant 2.65625 is 255/96, which is the
 * WM8758 span. On the Classic it left the bottom 31% of the range
 * unreachable and overflowed a byte above +6 dB, so the loudest settings
 * were reported to the accessory as near silence.
 */
static unsigned char iap_scale_volume(int volume, int lo, int hi)
{
    int span = hi - lo;

    if (span <= 0)
        return 0;
    if (volume <= lo)
        return 0;
    if (volume >= hi)
        return 255;

    return (unsigned char)(((volume - lo) * 255 + span / 2) / span);
}

/* Absolute volume: the device's own full range, "not normalized". */
unsigned char iap_volume_to_byte(int volume)
{
    return iap_scale_volume(volume, sound_min(SOUND_VOLUME),
                            sound_max(SOUND_VOLUME));
}

/* UI volume, which MFi Table 4-61 (p.261) says is "normalized to volume
 * limit settings", and the paragraph below it makes explicit: "setting
 * the UI volume to 255 will result in the Absolute volume being set to
 * the Apple device's Volume Limit setting."
 *
 * Rockbox's global_settings.volume_limit is that setting -- setvol()
 * (apps/misc.c:880) clamps the applied volume to it. It defaults to
 * sound_max, so on a default configuration this is the absolute scale
 * and nothing changes; once a user lowers the limit, their own maximum
 * has to report 255 rather than something short of it.
 */
unsigned char iap_volume_to_ui_byte(int volume)
{
    int lo = sound_min(SOUND_VOLUME);
    int limit = global_settings.volume_limit;

    if (limit > sound_max(SOUND_VOLUME))
        limit = sound_max(SOUND_VOLUME);
    if (limit <= lo)
        return 0;

    return iap_scale_volume(volume, lo, limit);
}

/* The inverse of the absolute scale: the device's whole range, matching
 * iap_volume_to_byte(). SetiPodStateInfo info type 0x10 carries one of
 * these in byte 2, and MFi Table 4-74 (p.269) makes it the value to use
 * when the UI byte before it is zero. */
int iap_byte_to_abs_volume(unsigned char level)
{
    int lo = sound_min(SOUND_VOLUME);
    int span = sound_max(SOUND_VOLUME) - lo;

    if (span <= 0)
        return lo;

    return lo + (level * span + 127) / 255;
}

/* The inverse of the UI scale, because that is what an accessory sends:
 * SetiPodStateInfo info types 0x04 and 0x10 both carry a UI volume. */
int iap_byte_to_volume(unsigned char level)
{
    int lo = sound_min(SOUND_VOLUME);
    int limit = global_settings.volume_limit;
    int span;

    if (limit > sound_max(SOUND_VOLUME))
        limit = sound_max(SOUND_VOLUME);
    span = limit - lo;

    if (span <= 0)
        return lo;

    return lo + (level * span + 127) / 255;
}

/* This thread is waiting for events posted to iap_queue and calls
 * the appropriate subroutines in response
 */
static void iap_thread(void)
{
    struct queue_event ev;
    while(1) {
        queue_wait(&iap_queue, &ev);
        switch (ev.id)
        {
            /* Handle the regular 100ms tick used for driving the
             * authentication state machine and notifications
             */
            case IAP_EV_TICK:
            {
                iap_periodic();
                /* pick up packets whose IAP_EV_MSG_RCVD event was lost
                   to a queue overflow, they would otherwise sit in the
                   RX buffer until the next packet arrives */
                iap_handlepkt();
                break;
            }

            /* Handle a newly received message from the device */
            case IAP_EV_MSG_RCVD:
            {
                iap_handlepkt();
                break;
            }

            case IAP_EV_ARTWORK:
            {
                iap_artwork_send_next((uint32_t)ev.data);
                break;
            }

            /* Handle memory allocation. This is used only once, during
             * startup
             */
            case IAP_EV_MALLOC:
            {
                iap_malloc();
                break;
            }

            /* Handle poweroff message */
            case SYS_POWEROFF:
            case SYS_REBOOT:
            {
                iap_shutdown = true;
                break;
            }

            /* Ack USB thread */
            case SYS_USB_CONNECTED:
            {
                usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
                break;
            }
        }
    }
}

/* called by playback when the next track starts */
static void iap_track_changed(unsigned short id, void *param)
{
    (void)id;

#ifdef USB_ENABLE_AUDIO
    struct track_event *te = param;
    unsigned long frequency = mixer_get_frequency();

    if (global_settings.play_frequency)
        frequency = global_settings.play_frequency;
    else if (te && te->id3 && te->id3->frequency)
        frequency = (te->id3->frequency % 4000) ? SAMPR_44 : SAMPR_48;

    if (DEVICE_LINGO_SUPPORTED(0x0A)
        && iap_audio_reported_frequency != frequency)
    {
        iap_audio_pending_frequency = frequency;
        if (!device.audio_init_pending)
        {
            device.audio_init_pending = true;
            queue_post(&iap_queue, IAP_EV_TICK, 0);
        }
    }
    else if (DEVICE_LINGO_SUPPORTED(0x0A) && iap_audio_reported_frequency)
    {
        /* Equivalent mutant: the frequency is only ever set by the caps
         * exchange, which an accessory without the lingo never reaches,
         * so the second term already excludes it. */
        /* Same rate, new track. Table 4-233 (p.346) has 1.01 correct
         * "a bug where TrackNewAudioAttributes was not being sent
         * before every track", so the rate not having moved is not a
         * reason to stay quiet. The caps exchange is not repeated --
         * the accessory's list has not changed. */
        device.audio_attrs_pending = true;
        queue_post(&iap_queue, IAP_EV_TICK, 0);
    }
#else
    (void)param;
#endif

    /* Table 5-47 (p.426) type 0x01 "Track index", which Table 5-45
     * (p.425) bit 02 subscribes to. This used to ride on do_notify,
     * which the Display Remote lingo also writes. */
    if ((interface_state == IST_EXTENDED)
        && (device.pb_notifications & BIT_N(2))) {
        /* Flagged, not sent. firmware/events.c send_event() runs its
         * handlers synchronously in the caller's thread, and this one
         * is fired from apps/playback.c:2622 on the audio thread -- so
         * building a packet here writes iap_txnext and the TX payload
         * out from under whatever the iAP thread is assembling.
         *
         * The iAP thread demonstrably holds that buffer across blocking
         * calls: iap-lingo3.c builds GetPlayStatus and then takes
         * id3_mutex through audio_current_track(), which the audio
         * thread holds while firing this very event, and builds
         * GetIndexedPlayingTrackInfo and then reads metadata off disk.
         * Under USB HID the window is unconditional -- iap_hid_tx()
         * blocks per fragment on a mutex and a 20 ms semaphore, reading
         * each chunk out of the shared buffer between waits.
         *
         * The result was one packet carrying this notification's bytes
         * followed by the tail of a Display Remote reply, under a
         * single length and checksum, and the reply the accessory asked
         * for never arriving.
         *
         * The Digital Audio half of this same function already does it
         * this way, a few lines above. */
        device.pb_track_changed = true;
        queue_post(&iap_queue, IAP_EV_TICK, 0);
        return;
    }
}

/* Set up the IAP infrastructure.
 *
 * On the first call (boot), creates the message queue, handler thread
 * and notification timer.  On subsequent calls (e.g. USB HID reconnect)
 * only resets the device state, avoiding duplicate threads.
 */
void iap_setup(const int ratenum)
{
    if (ratenum != IAP_RATE_UNCHANGED)
        iap_bitrate_set(ratenum);
    iap_remotebtn = BUTTON_NONE;
    iap_setupflag = true;
    iap_running = false;

    if (!iap_started)
    {
        unsigned int tid;

        iap_reset_device(&device);
        queue_init(&iap_queue, true);
        tid = create_thread(iap_thread, thread_stack, sizeof(thread_stack),
                0, "iap"
                IF_PRIO(, PRIORITY_SYSTEM)
                IF_COP(, CPU));
        if (!tid)
            panicf("Could not create iap thread");
        timeout_register(&iap_task_tmo, iap_task, MS_TO_TICKS(100),
                (intptr_t)NULL);
        add_event(PLAYBACK_EVENT_TRACK_CHANGE, iap_track_changed);
        iap_started = true;
    }
    else
    {
        iap_reset_device(&device);
    }

    iap_apply_pending_interface_pause();
}

/* Trigger buffer allocation for the IAP thread.
 *
 * Called from iap_getc() on the first received sync byte.
 * May run in interrupt context (serial UART ISR), so only
 * queue_post() is used here -- the thread and queue were
 * already created by iap_setup().
 */
static void iap_start(void)
{
    queue_post(&iap_queue, IAP_EV_MALLOC, 0);
}

void iap_malloc(void)
{
#ifndef IAP_MALLOC_DYNAMIC
    static unsigned char serbuf[IAP_MALLOC_SIZE];
#endif

    if (iap_running)
        return;

#ifdef IAP_MALLOC_DYNAMIC
    iap_buffer_handle = core_alloc_ex(IAP_MALLOC_SIZE, &iap_buflib_callbacks);
    if (iap_buffer_handle < 0)
        panicf("Could not allocate buffer memory");
    iap_buffers = core_get_data(iap_buffer_handle);
#else
    iap_buffers = serbuf;
#endif

    iap_reset_buffers();
    iap_running = true;
}

void iap_bitrate_set(const int ratenum)
{
    switch(ratenum)
    {
        case 0:
            serial_bitrate(0);
            break;
        case 1:
            serial_bitrate(9600);
            break;
        case 2:
            serial_bitrate(19200);
            break;
        case 3:
            serial_bitrate(38400);
            break;
        case 4:
            serial_bitrate(57600);
            break;
    }
}

/* Message format:
   0xff
   0x55
   length
   mode
   command (2 bytes)
   parameters (0-n bytes)
   checksum (length+mode+parameters+checksum == 0)
*/

/* Send the current content of the TX buffer.
 * This will check for TX buffer overflow and panic, but it might
 * be too late by then (although one would have to overflow the complete
 * RX buffer as well)
 */
void iap_send_tx(void)
{
    int i, chksum;
    ptrdiff_t txlen;
    unsigned char* txstart;

    txlen = iap_txnext - iap_txpayload;

    if (txlen <= 0)
        return;

    if (txlen > TX_BUFLEN)
        panicf("IAP: TX buffer overflow");

    /* MFi 2.5.2: the 1-byte length field expresses a payload of 0x02 to
     * 0xFC. 253..255 must use the 3-byte form, which the old < 256 test
     * emitted as a short packet the accessory cannot parse. Every
     * transaction ID added to a reply moves it two bytes nearer this. */
    if (txlen <= 0xFC)
    {
        /* Short packet */
        txstart = iap_txstart+2;
        *(txstart+2) = txlen;
        chksum = txlen;
    } else {
        /* Long packet */
        txstart = iap_txstart;
        *(txstart+2) = 0x00;
        *(txstart+3) = (txlen >> 8) & 0xFF;
        *(txstart+4) = (txlen) & 0xFF;
        chksum = *(txstart+3) + *(txstart+4);
    }
    *(txstart) = 0xFF;
    *(txstart+1) = 0x55;

    for (i=0; i<txlen; i++)
    {
        chksum += iap_txpayload[i];
    }
    *(iap_txnext) = 0x100 - (chksum & 0xFF);

#if defined(LOGF_ENABLE) && defined(ROCKBOX_HAS_LOGF)
    logf("T: %s", hexstring(txstart+3, (iap_txnext - txstart)-3));
#endif
    /* Closed before the send, not after.
     *
     * The packet is fully built by here and the length is already
     * computed, so the buffer is no longer being written -- and
     * iap_transport_send() yields on the 6G, where iap_hid_tx() takes
     * tx_frame_lock and then waits on a semaphore per fragment.
     * Resetting afterwards left iap_txnext past the payload across that
     * yield, so anything the other side ran during it saw a packet
     * half-built when there was none.
     *
     * Saying so at all matters because "is a packet half-built right
     * now" is what the blocking-call rule turns on: without the reset,
     * iap_txnext stays past the payload until the next IAP_TX_INIT, an
     * IAP_TX_PUT that forgot its init would append to bytes already on
     * the wire, and the question has no answer. The test harness asks
     * it; see the guard on the blocking stubs in test/rb_stubs.c. */
    {
        int txbytes = (iap_txnext - txstart) + 1;

        iap_txnext = iap_txpayload;
        iap_transport_send(txstart, txbytes);
    }
}

/* This is just a compatibility wrapper around the new TX buffer
 * infrastructure
 */
void iap_send_pkt(const unsigned char * data, const int len)
{
    int hdr;

    if (!iap_running)
        return;

    /* MFi p.95: "After an Apple device has successfully acknowledged an
     * accessory's StartIDPS command, all subsequent iAP command packets
     * must include transaction IDs, regardless of lingo."
     *
     * This copied the caller's bytes through untouched, so the tuner
     * driver's fourteen commands went out without one -- while
     * iap-lingo7.c builds the very same commands with
     * IAP_TX_PUT_IPOD_TRANSID(), so 07 05 could go out both ways in a
     * single session. It stayed hidden while radio_present was only set
     * on the legacy identify path; once an IDPS accessory could have a
     * radio, opening the FM screen put a dozen malformed frames on the
     * wire and the radio did not work at all.
     *
     * These are commands this device originates, so they carry its own
     * counter. The header is the lingo plus the command ID -- two bytes,
     * or three for Extended Interface (MFi Table 2-10, p.109) -- and the
     * ID goes straight after it, which is where every handler that
     * builds one of these by hand puts it. */
    hdr = ((len >= 1) && (data[0] == 0x04)) ? 3 : 2;

    iap_txnext = iap_txpayload;
    if (DEVICE_TRANSID_ACTIVE && len >= hdr)
    {
        IAP_TX_PUT_DATA(data, hdr);
        IAP_TX_PUT_IPOD_TRANSID();
        IAP_TX_PUT_DATA(data + hdr, len - hdr);
    }
    else
    {
        IAP_TX_PUT_DATA(data, len);
    }
    iap_send_tx();
}

/* Send a prebuilt reply, inserting the transaction ID of the command
 * being answered. MFi 2.6.1.4: the ID goes "after the Command ID
 * field", which is two bytes for the Extended Interface lingo and one
 * for every other (Table 2-10), so the insertion point comes from the
 * lingo in data[0]. With no transaction ID in play this is exactly
 * iap_send_pkt().
 */
void iap_send_reply(const unsigned char * data, const int len,
                    unsigned char tid_hi, unsigned char tid_lo)
{
    int hdr;

    if (!iap_running)
        return;

    if (!DEVICE_TRANSID_ACTIVE)
    {
        iap_send_pkt(data, len);
        return;
    }

    hdr = ((len >= 1) && (data[0] == 0x04)) ? 3 : 2;
    if (len < hdr)
    {
        iap_send_pkt(data, len);
        return;
    }

    iap_txnext = iap_txpayload;
    IAP_TX_PUT_DATA(data, hdr);
    IAP_TX_PUT(tid_hi);
    IAP_TX_PUT(tid_lo);
    IAP_TX_PUT_DATA(data + hdr, len - hdr);
    iap_send_tx();
}

bool iap_getc(IF_IAP_MP(int port,) const unsigned char x)
{
    struct state_t *s = &frame_state;
    static long pkt_timeout;

    /* Report "still hunting" while IAP is not set up yet, otherwise
     * the serial driver would lock its autobaud detection onto the
     * default bitrate before any real traffic was seen.
     */
    if (!iap_setupflag)
        return true;

    /* Check the time since the last packet arrived. */
    if ((s->state != ST_SYNC) && TIME_AFTER(current_tick, pkt_timeout)) {
        /* Packet timeouts only make sense while not waiting for the
         * sync byte */
         s->state = ST_SYNC;
         return iap_getc(IF_IAP_MP(port,) x);
    }


    /* run state machine to detect and extract a valid frame */
    switch (s->state) {
    case ST_SYNC:
        if (x == 0xFF) {
            /* The IAP infrastructure is started by the first received sync
             * byte. It takes a while to spin up, so do not advance the state
             * machine until it has started.
             */
            if (!iap_running)
            {
                iap_start();
                break;
            }
            iap_rxnext = iap_rxpayload;
            s->state = ST_SOF;
        }
        break;
    case ST_SOF:
        if (x == 0x55) {
            /* received a valid sync/SOF pair */
            s->state = ST_LEN;
        } else {
            s->state = ST_SYNC;
            return iap_getc(IF_IAP_MP(port,) x);
        }
        break;
    case ST_LEN:
        s->check = x;
        s->count = 0;
        if (x == 0) {
            /* large packet */
            s->state = ST_LENH;
        } else {
            /* small packet.
             * Compare by addition: iap_rxlen is uint32_t, so the old
             * (iap_rxlen-2) wrapped to ~4G once the buffer filled to
             * within one byte, and then accepted every frame.
             */
            if ((uint32_t)x + 2 > iap_rxlen)
            {
                /* Packet too long for buffer */
                s->state = ST_SYNC;
                break;
            }
            /* MFi 2.5.2 (p.110): "A 1-byte field can express a payload
             * length of 0x02 to 0xFC (2 to 252) in a single byte."
             * Only zero was rejected, so a one-byte payload was framed
             * and dispatched -- and every lingo handler reads its
             * command from buf[1], one past such a packet, out of the
             * buffer's untouched tail. In the General lingo that byte
             * then names the command a rejection is addressed to, and a
             * rejection with no transaction ID is a 4-byte iPodAck,
             * which 2.6.1.2 (p.111) makes the signal to stop using
             * transaction IDs. 0xFD upwards must use the 3-byte form. */
            if (x < 0x02 || x > 0xFC)
            {
                s->state = ST_SYNC;
                break;
            }
            s->len = x;
            s->state = ST_DATA;
            put_u16(iap_rxnext, s->len);
            iap_rxnext += 2;
        }
        break;
    case ST_LENH:
        s->check += x;
        s->len = x << 8;
        s->state = ST_LENL;
        break;
    case ST_LENL:
        s->check += x;
        s->len += x;
        /* MFi 2.5.2 (p.110): the 3-byte form carries "a 2-byte payload
         * length value from 0x00FD to 0xFFFA (253 to 65529)". Anything
         * below that belongs in the 1-byte form. Only zero was
         * rejected. */
        if ((s->len < 0x00FD) || (s->len > 0xFFFA)
            || ((uint32_t)s->len + 2 > iap_rxlen)) {
            /* invalid length */
            s->state = ST_SYNC;
            break;
        } else {
            s->state = ST_DATA;
            put_u16(iap_rxnext, s->len);
            iap_rxnext += 2;
        }
        break;
    case ST_DATA:
        s->check += x;
        *(iap_rxnext++) = x;
        s->count += 1;
        if (s->count == s->len) {
            s->state = ST_CHECK;
        }
        break;
    case ST_CHECK:
        s->check += x;
        if ((s->check & 0xFF) == 0) {
            /* done, received a valid frame */
            iap_rxlen -= (s->len + 2);
            iap_rxpayload = iap_rxnext;
            queue_post(&iap_queue, IAP_EV_MSG_RCVD, 0);
        } else {
            /* Invalid frame */
        }
        s->state = ST_SYNC;
        break;
    default:
#ifdef LOGF_ENABLE
           logf("Unhandled iap state %d", (int) s->state);
#else
           panicf("Unhandled iap state %d", (int) s->state);
#endif
        break;
    }

    pkt_timeout = current_tick + IAP_PKT_TIMEOUT;

    /* return true while still hunting for the sync and start-of-frame byte */
    return (s->state == ST_SYNC) || (s->state == ST_SOF);
}

bool iap_get_trackinfo(const unsigned int track, struct mp3entry* id3)
{
    long tracknum;
    struct playlist_track_info info;

    if (track >= (uint32_t)playlist_amount())
        return false;

    /* The conversion and the check against it have to be the same one.
     * All three callers probed playlist_get_track_info() with the
     * accessory's index first and then called this, which converts --
     * so for any playlist whose first index is not zero, the track that
     * was checked and the track that was read were different tracks,
     * and a probe that passed said nothing about the read that
     * followed. Doing both here leaves nothing to get out of step
     * with, and none of the three used the probe's struct for
     * anything but its return value. */
    tracknum = iap_track_from_mfi(track);

    /* Equivalent mutant: the disk path below reaches
     * playlist_get_track_info(), whose get_track_filename() refuses
     * index < 0 || index >= amount (apps/playlist.c:271), and the fast
     * path cannot be taken by an out-of-range index because
     * iap_current_track_index() only ever returns an in-range one. Kept
     * because this is where the caller's index becomes a playlist
     * index, so it is where the range it has to be in is worth
     * stating, and because it is the check the three callers used to
     * do for themselves. */
    if (tracknum < 0 || tracknum >= (long)playlist_amount())
        return false;

    /* If the tracknumber is not the current one,
       read id3 from disk */
    if(iap_current_track_index() != tracknum)
    {
        /* And the read has to have worked. Ignoring the return left
         * info.filename holding whatever was on the stack, which
         * get_metadata() then opened; the sibling site in
         * iap-lingo4.c's GetIndexedPlayingTrackTitle already checks
         * it. get_metadata() itself returns false on a parse or open
         * failure after wiping the entry, so that is the caller's
         * answer too. */
        if (playlist_get_track_info(NULL, (int)tracknum, &info) < 0)
            return false;
        /* memset(id3, 0, sizeof(*id3)) --get_metadata does this for us */
        return get_metadata(id3, -1, info.filename);
    }

    memcpy(id3, audio_current_track(), sizeof(*id3));
    return true;
}

bool iap_chapter_at(const struct mp3entry *id3, uint32_t index,
                    struct iap_chapter_info *chapter)
{
    struct cuesheet *cue;
    unsigned long start, end;

    if (!id3 || !chapter || !(cue = id3->cuesheet)
        || cue->track_count <= 0 || index >= (uint32_t)cue->track_count)
        return false;

    start = cue->tracks[index].offset;
    end = index + 1 < (uint32_t)cue->track_count
        ? cue->tracks[index + 1].offset : id3->length;
    if (end < start)
        end = start;

    chapter->index = index;
    chapter->count = cue->track_count;
    chapter->offset_ms = start;
    chapter->length_ms = end - start;
    chapter->elapsed_ms = id3->elapsed > start ? id3->elapsed - start : 0;
    if (chapter->elapsed_ms > chapter->length_ms)
        chapter->elapsed_ms = chapter->length_ms;
    chapter->name = cue->tracks[index].title;
    return true;
}

bool iap_current_chapter(const struct mp3entry *id3,
                         struct iap_chapter_info *chapter)
{
    struct cuesheet *cue;
    uint32_t index = 0;

    if (!id3 || !(cue = id3->cuesheet) || cue->track_count <= 0)
        return false;

    while (index + 1 < (uint32_t)cue->track_count
           && cue->tracks[index + 1].offset <= id3->elapsed)
        index++;

    return iap_chapter_at(id3, index, chapter);
}

bool iap_set_chapter(uint32_t index)
{
    struct iap_chapter_info chapter;
    struct mp3entry *id3;

    if (!(audio_status() & AUDIO_STATUS_PLAY))
        return false;

    id3 = audio_current_track();
    if (!iap_chapter_at(id3, index, &chapter))
        return false;

    audio_ff_rewind(chapter.offset_ms);
    return true;
}

bool iap_skip_chapter(int direction)
{
    struct iap_chapter_info chapter;
    struct iap_chapter_info target_chapter;
    struct mp3entry *id3;
    uint32_t target;

    if (!(audio_status() & AUDIO_STATUS_PLAY))
        return false;

    id3 = audio_current_track();
    if (!iap_current_chapter(id3, &chapter))
        return false;

    if (direction > 0) {
        if (chapter.index + 1 >= chapter.count)
            return false;
        target = chapter.index + 1;
    } else {
        if (chapter.elapsed_ms > 2000)
            target = chapter.index;
        else if (chapter.index > 0)
            target = chapter.index - 1;
        else
            return false;
    }

    if (!iap_chapter_at(id3, target, &target_chapter))
        return false;

    audio_ff_rewind(target_chapter.offset_ms);
    return true;
}

/* An accessory's mute, in one place because the pending unmute has to
 * move with it.
 *
 * There is no mute in the app-level sound API, so muting drives the
 * hardware down with sound_set_volume(sound_min()) and keeps the
 * user's level in global_status.volume -- sound_set_volume() writes
 * that itself (firmware/sound.c:320), so the level to come back to has
 * to be put back after it. Unmuting goes through setvol()
 * (apps/misc.c:871), which is what the wheel, the sound menu and the
 * radio screen all use, and which re-applies the stored level.
 *
 * Two copies of this lived in iap-lingo3.c, and neither knew about
 * device.unmute_pending -- the flag iap_reset_device() sets when a
 * muted accessory goes away, so that iap_periodic() can call setvol()
 * off the tick. A detach followed by a new accessory that mutes left
 * the flag set behind a live mute, and the next iap_periodic() lifted
 * it: setvol() ran while device.mute stayed true, so the device
 * reported muted and played at the user's level with nothing to say
 * why. Whatever an accessory asks for now is the current truth, so the
 * owed unmute is spent either way.
 */
/* "Playing (for Command 0x0E: SetiPodStateInfo (page 266), start or
 * resume playback)" -- Table 4-62 (p.262), and the parenthesis is the
 * whole point: for that command the value has to START playback, not
 * only resume it.
 *
 * audio_resume() cannot. apps/playback.c:3187 begins
 * "if (play_status == PLAY_STOPPED || ...) return;", so on a stopped
 * engine it is a no-op -- and iap-lingo3.c acknowledged Success. That
 * is the Play button of every head unit that speaks only General and
 * Display Remote, in the ordinary case of docking an idle iPod.
 *
 * iap-lingo4.c's SetCurrentPlayingTrack learned the same lesson in
 * 514204a8c6 and starts the playlist instead; this is that fix with a
 * name, so the two cannot drift again.
 *
 * Returns false when there is nothing to start, which MFi 4.3.17
 * (p.266) turns into an iPodAck carrying the result of the operation
 * rather than a Success that did not happen.
 */
bool iap_play_or_resume(void)
{
    if (iap_play_state_byte() != 0x00)
    {
        audio_resume();
        return true;
    }

    if (playlist_amount() <= 0)
        return false;

    /* No re-shuffle and no re-sort. iap-lingo4.c does both because it
     * is about to jump to a chosen index and the queue order has to
     * mean something first; this command chooses nothing, so touching
     * the user's queue order would be an edit nobody asked for. */
    playlist_start(iap_current_track_index(), 0, 0);
    return true;
}

/* A reorder changes the playback engine's contents without changing
 * how many tracks are in it.
 *
 * Both contents-changed events -- Table 5-45 bit 12 / type 0x0E, and
 * Table 4-59 bit 18 / event 0x12 -- compared playlist_amount() and
 * nothing else, and apps/playlist.c permutes indices[] in place, so
 * shuffling or sorting raised neither. MFi 5.1.42 (p.432) is explicit
 * about what changed: "Shuffling tracks does not affect the track
 * index, just the track at that index", and 1.11.2.1.2 (p.59) has an
 * accessory that draws a list respond "to every 'Playback engine
 * contents changed' notification" with polling ruled out.
 *
 * Watched here rather than flagged at each reorder call site, because
 * the ones that matter most are not in this tree: a user toggling
 * Shuffle in Rockbox's own menu reorders the queue with the iAP layer
 * uninvolved, and a head unit's cached list is wrong from that moment.
 * playlist_get_seed() (apps/playlist.c) changes on every randomise
 * whoever asked for it; playlist_sort() leaves the seed alone, which is
 * what the shuffle-setting term is for.
 */
static bool contents_changed(void)
{
    unsigned int seed = (unsigned int)playlist_get_seed(NULL);
    bool shuffled = !!global_settings.playlist_shuffle;

    if (seed == contents_seed && shuffled == contents_shuffled)
        return false;

    contents_seed = seed;
    contents_shuffled = shuffled;
    return true;
}

void iap_set_mute(bool mute)
{
    if (mute) {
        if (!device.mute) {
            int keep = global_status.volume;
            sound_set_volume(sound_min(SOUND_VOLUME));
            global_status.volume = keep;
        }
    } else {
        setvol();
    }

    device.mute = mute;
    device.unmute_pending = false;
}

uint32_t iap_get_trackpos(void)
{
    struct mp3entry *id3 = audio_current_track();

    return id3->elapsed;
}

/* The accessory's index space and Rockbox's differ by first_index, and
 * both wrap at the end of the queue. Written out at each site, one copy
 * lost its wrap and the same device reported 4294967294 on one lingo
 * and 8 on another for the same track; the fix reached three of the
 * five copies. Two functions, so there is one place to be wrong. */
/* The playback state byte, Table 4-62 (p.262): 0x00 stopped, 0x01
 * playing, 0x02 paused.
 *
 * Three sites encoded this separately and two of them disagreed. With
 * recording paused -- AUDIO_STATUS_RECORD | AUDIO_STATUS_PAUSE, which
 * both targets can reach, HAVE_RECORDING is set for each -- Display
 * Remote tested the PLAY bit and answered stopped while Extended
 * Interface tested the PAUSE bit and answered paused. Extended
 * Interface also had no else at all, so a stopped player left the byte
 * at whatever its buffer was initialised to.
 *
 * Stopped is the answer: this reports playback, and recording is not
 * playback. */
unsigned char iap_play_state_byte(void)
{
    int status = audio_status();

    if (!(status & AUDIO_STATUS_PLAY))
        return 0x00;
    if (status & AUDIO_STATUS_PAUSE)
        return 0x02;
    return 0x01;
}

/* The Display Remote play status, which has two states more than the
 * play/pause/stop the gates use.
 *
 * MFi Table 4-62 (p.262): "0x00 Playback stopped, 0x01 Playing, 0x02
 * Playback paused, 0x03 Fast forward (FF), 0x04 Fast rewind (REW), 0x05
 * End fast forward or rewind mode." Three sites reported this and all
 * three used iap_play_state_byte(), which stops at 0x02 -- one of them
 * with a "TODO: Handle FF/REW" on it. An accessory seeking saw plain
 * Playing, so its display never showed the seek it had asked for.
 *
 * device.pb_seeking already holds the direction, set by
 * iap_seek_start() for whichever lingo asked. Kept separate from
 * iap_play_state_byte() because that one is a gate -- seven callers
 * test it against 0x00 to mean "nothing is playing" -- and widening it
 * would make them read a seek as some fourth thing. */
unsigned char iap_play_state_reported(void)
{
    if (device.pb_seeking == 0x02)      /* FFW, Table 5-47 type 0x02 */
        return 0x03;
    if (device.pb_seeking == 0x03)      /* REW */
        return 0x04;

    return iap_play_state_byte();
}

uint32_t iap_track_to_mfi(long rb_index)
{
    long pos = rb_index - playlist_get_first_index(NULL);

    if (pos < 0)
        pos += playlist_amount();

    return (uint32_t)pos;
}

long iap_track_from_mfi(uint32_t mfi_index)
{
    long pos = (long)mfi_index + playlist_get_first_index(NULL);

    if (pos >= (long)playlist_amount())
        pos -= playlist_amount();

    return pos;
}

/* Which track is playing, as a playlist index.
 *
 * playlist_next(0) answers the same question and four sites used it to
 * ask. It is the wrong call for a query twice over. It blocks: its
 * first statement is dc_thread_stop(), which under HAVE_DIRCACHE -- on
 * for both targets -- is a queue_send() to the playlist thread, so an
 * unconditional block_thread()/switch_thread(). And it mutates: it
 * assigns playlist->index, and on its index < 0 path re-shuffles the
 * queue, forces global_settings.playlist_shuffle true under
 * REPEAT_SHUFFLE, or calls create_and_play_dir().
 *
 * The values agree for any current track that is not PLAYLIST_SKIPPED:
 * calculate_step_count(playlist, 0) returns 0 for one, and every
 * repeat branch of get_next_index() is then the identity. Where they
 * disagree, playlist_next() is answering a different question -- which
 * track comes next past a bad entry -- and this is the one the callers
 * meant. */
/* Wake the iAP thread, so a flag another thread set is acted on now
 * rather than at the next tick. iap_queue is static here on purpose --
 * the queue is this thread's, and the only thing another thread should
 * be able to do with it is knock. */
void iap_wake(void)
{
    queue_post(&iap_queue, IAP_EV_TICK, 0);
}

void iap_schedule_artwork(uint32_t transfer_id)
{
    queue_post(&iap_queue, IAP_EV_ARTWORK, transfer_id);
}

/* TrackNewAudioAttributes (0x0A/0x04), Table 4-232 (p.346).
 *
 * Digital Audio lingo version 1.01, Table 4-233 (p.346): "The
 * TrackNewAudioAttributes command is resent until it is acknowledged by
 * the USB host with an AccessoryAck command. Also corrected a bug where
 * TrackNewAudioAttributes was not being sent before every track; for
 * this reason, accessories should not use Digital Audio with Apple
 * devices that support only Digital Audio lingo version 1.00."
 *
 * Both clauses were missing, which is why this device advertised 1.00
 * -- and 4.10 (p.345) tells accessories what to do with that: "An
 * accessory should check the attached Apple device's version of the
 * Digital Audio lingo and use digital audio only if the version number
 * is greater than 1.00." A conformant dock therefore never streamed.
 *
 * The payload is fixed apart from the sample rate, which the caps
 * exchange has already chosen and left in iap_audio_reported_frequency,
 * so a resend does not need the accessory's rate list again.
 */
void iap_send_audio_attrs(void)
{
    /* Equivalent mutant, and so is the identical check at the top of
     * iap_handlepkt_mode10(): each is the other's backstop, so removing
     * either alone changes nothing the suite can see. Both are kept --
     * one guards the send, one guards the handler, and the sweep cannot
     * tell defence in depth from dead code. */
    if (!DEVICE_LINGO_SUPPORTED(0x0A) || !DEVICE_AUTHENTICATED)
        return;

    IAP_TX_INIT(0x0A, 0x04);
    IAP_TX_PUT_IPOD_TRANSID();
#ifdef USB_ENABLE_AUDIO
    IAP_TX_PUT_U32(iap_audio_reported_frequency);
#else
    IAP_TX_PUT_U32(44100);
#endif
    IAP_TX_PUT_U32(0);      /* sound check value */
    IAP_TX_PUT_U32(0);      /* volume adjustment */
    iap_send_tx();

    audio_attrs_unacked = true;
}

/* Start and stop a seek.
 *
 * Two lingoes ask for one. Extended Interface's PlayControl has Begin
 * FF, Begin REW and End FF/REW (Table 5-48, p.429), and Display
 * Remote's SetiPodStateInfo play status has the same three -- Table
 * 4-62 (p.262): "0x03 Fast forward (FF), 0x04 Fast rewind (REW), 0x05
 * End fast forward or rewind mode", reached through info type 0x03,
 * which Table 4-74 (p.267) describes as "The play status of the Apple
 * device (play, pause, stop, FF or REW)".
 *
 * Only PlayControl implemented it, so a Display Remote head unit -- one
 * that never negotiated lingo 4 -- had no seek at all: the values were
 * answered Command Failed. Putting the mechanism here rather than
 * copying it means the notifications, the hold timer and the seek state
 * cannot drift apart between the two.
 */
bool iap_seek_start(bool forward)
{
    /* Nothing to seek through if nothing is playing.
     *
     * MFi 5.1.37 (p.428): "If the Apple device does not enter the
     * requested state successfully, an error status is returned." The
     * skip arms beside PlayControl's seek arms already test this; the
     * seek arms did not, and a seek is worse than a skip when it is
     * wrong. It holds the button for IAP_BTN_HELD -- about ten seconds
     * -- and button-clickwheel.c:479 returns "int_btn |
     * remote_control_rx()", so BUTTON_RC_RIGHT is ORed into every
     * physical button read for that whole time. keymap-ipod.c maps it
     * to ACTION_STD_NEXT in the browser, so a seek sent to a stopped
     * device scrolls the user's file list on its own until the timer
     * lapses. */
    if (iap_play_state_byte() == 0x00)
        return false;

    iap_remotebtn = forward ? BUTTON_RC_RIGHT : BUTTON_RC_LEFT;
    iap_repeatbtn = 2;
    iap_timeoutbtn = IAP_BTN_HELD;
    device.pb_seeking = forward ? 0x02 : 0x03;

    /* Table 5-47 (p.427) type 0x06: 0x05 FFW seek started, 0x06 REW. */
    iap_send_pb_extended(forward ? 0x05 : 0x06);
    return true;
}

void iap_seek_stop(void)
{
    iap_remotebtn = BUTTON_NONE;
    iap_repeatbtn = 2;
    iap_timeoutbtn = 0;

    /* Table 5-45 (p.425) bit 00 covers "stop, FFW seek stop, or REW
     * seek stop, using status notification types 0x00, 0x02, or 0x03",
     * and Table 5-47 (p.426) gives both seek-stop types no parameter. */
    if ((interface_state == IST_EXTENDED)
        && device.pb_seeking
        && (device.pb_notifications & BIT_N(0))) {
        IAP_TX_INIT4(0x04, 0x0027);
        IAP_TX_PUT_IPOD_TRANSID();
        IAP_TX_PUT(device.pb_seeking);
        iap_send_tx();
    }
    if (device.pb_seeking)
        iap_send_pb_extended(0x07);     /* seek stopped */
    device.pb_seeking = 0;
}

/* PlayStatusChangeNotification type 0x06, "Playback status extended".
 *
 * MFi Table 5-45 (p.425) bit 01: "Extended play state changes (playback
 * stop, FFW seek start, REW seek start, playback started, FFW/REW seek
 * stop, or playback pause using status notification type 0x06)." Table
 * 5-47 (p.427) gives the codes: 0x02 Stopped, 0x05 FFW seek started,
 * 0x06 REW seek started, 0x07 FFW/REW seek stopped, 0x0A Playing,
 * 0x0B Paused.
 *
 * Nothing sent it. The mask was stored whole and acked Success, so an
 * accessory subscribing to bit 01 was told yes and heard nothing -- and
 * bit 01 is the only asynchronous route to play and pause. Bit 00
 * covers stop and seek-stop only, and the one-byte form maps to bits 0,
 * 2, 3 and 5, so a lingo 0 + 4 head unit had no way at all to learn
 * that the user had paused. Same defect as bit 00 had before a16d055502.
 */
void iap_send_pb_extended(unsigned char state)
{
    if (interface_state != IST_EXTENDED)
        return;
    if (!(device.pb_notifications & BIT_N(1)))
        return;

    IAP_TX_INIT4(0x04, 0x0027);
    IAP_TX_PUT_IPOD_TRANSID();
    IAP_TX_PUT(0x06);
    IAP_TX_PUT(state);
    iap_send_tx();
}

int iap_current_track_index(void)
{
    return playlist_get_current()->index;
}

uint32_t iap_get_trackindex(void)
{
    return iap_track_to_mfi(iap_current_track_index());
}

/* Fold in any change the notification path has not already accounted
 * for, and take the new values as reported.
 *
 * MFi 4.3.13 (p.263): "This command may be used to poll the Apple
 * device for certain event changes without enabling asynchronous remote
 * event notification."
 *
 * changed_notifications is written in exactly one place otherwise --
 * iap_periodic() -- and every write there sits inside a
 * "if (device.notifications & BIT_N(n))" block, behind an early return
 * taken when nothing is subscribed. So an accessory that takes the
 * spec at its word, never sends SetRemoteEventNotification and polls
 * 0x0A instead, read 00000000 for the life of the session and
 * concluded nothing on the device ever changes: no track change, no
 * play/pause, no volume. Its display never updated.
 *
 * The poll keeps its own copies rather than sharing the notification
 * path's. Sharing them looked right -- both are "what the accessory
 * knows" -- but they are not the same thing. RetRemoteEventStatus
 * carries a bitmask of what changed (Table 4-68, p.264) and never a
 * value, so a poll tells the accessory that the track moved and not
 * where to. The notification still owes it the number. Writing the
 * shared field from here consumed the change and the notification never
 * went out, which is worse than the bug it was fixing: an accessory
 * doing both lost the events entirely. */
/* Sample everything the poll watches, and say what moved.
 *
 * report is false when this is taking the session baseline, where the
 * values matter and the change bits do not. */
static void iap_poll_sample(bool report)
{
    uint32_t index         = iap_get_trackindex();
    struct iap_chapter_info chapter;
    struct mp3entry *id3   = audio_current_track();
    uint32_t chapter_index = ((audio_status() & AUDIO_STATUS_PLAY)
                              && iap_current_chapter(id3, &chapter))
                           ? chapter.index : UINT32_MAX;
    /* iap_play_state_reported(), not audio_status(): notification bit
     * 3 has to fire whenever the value the accessory would be told
     * changes, and that value is this one. audio_status() stays
     * AUDIO_STATUS_PLAY across a fast-forward, so a head unit that had
     * subscribed watched the state go 0x01 -> 0x03 -> 0x01 on the
     * device and heard about none of it -- its transport buttons and
     * its progress bar both stall until something else moves. */
    unsigned char status   = iap_play_state_reported();
    /* Same expression as the notification's, mute term included --
     * Table 4-61 (p.258): "if the Mute State value is true (mute is
     * on), the UI volume level field is not valid and is returned as
     * 0." */
    unsigned char level    = device.mute
                           ? 0 : iap_volume_to_ui_byte(global_status.volume);
    unsigned char shuffle  = global_settings.playlist_shuffle;
    unsigned char repeat   = global_settings.repeat_mode;

    unsigned char abs_level = iap_volume_to_byte(global_status.volume);
    uint32_t      pos_ms    = id3 ? id3->elapsed : 0;
    uint16_t      pos_s     = (pos_ms / 1000) & 0xFFFF;
    unsigned char power     = charger_input_state;
    unsigned char battery   = battery_level();
    unsigned char hold      = button_hold();
    uint32_t      ntracks   = (uint32_t)playlist_amount();

    if (!report)
        goto store;

    if (polled.track_index != index)  device.changed_notifications |= BIT_N(1);
    if (polled.play_status != status) device.changed_notifications |= BIT_N(3);
    if ((polled.volume != level) || (polled.mute != device.mute))
                                      device.changed_notifications |= BIT_N(4);
    if (polled.shuffle != shuffle)    device.changed_notifications |= BIT_N(7);
    if (polled.repeat != repeat)      device.changed_notifications |= BIT_N(8);

    /* The rest of what iap_periodic() tracks. MFi 4.3.13 (p.263) has
     * the answer carry "a bitmask of event states that changed since
     * the last GetRemoteEventStatus command" -- the whole bitmask, not
     * the five the notification path happened to share a shadow with.
     * Every other write to changed_notifications is inside a
     * "device.notifications & BIT_N(n)" block behind a subscription
     * early return, so an accessory that polls instead of subscribing
     * -- which the same paragraph exists to allow -- saw a still
     * elapsed-time bar and a battery icon that never moved. */
    if (polled.trackpos_ms != pos_ms)  device.changed_notifications |= BIT_N(0);
    if (polled.chapter_index != chapter_index)
                                       device.changed_notifications |= BIT_N(2);
    if ((polled.power_state != power) || (polled.battery_level != battery))
                                       device.changed_notifications |= BIT_N(5);
    if (polled.hold != hold)           device.changed_notifications |= BIT_N(0x0C);
    if (polled.numtracks != ntracks)   device.changed_notifications |= BIT_N(18);
    if (polled.trackpos_s != pos_s)    device.changed_notifications |= BIT_N(15);
    if ((polled.volume != level) || (polled.mute != device.mute)
        || (polled.abs_volume != abs_level))
                                       device.changed_notifications |= BIT_N(16);

    /* Bit 09, date and time, is deliberately absent. get_time() reads
     * the RTC over i2c once a second and takes i2c_mtx, and this runs
     * from GetRemoteEventStatus -- a yield there is the defect this
     * layer has been fixed for four times. An accessory that wants the
     * clock can subscribe to it; the notification path has no buffer
     * open when it reads. */

store:
    polled.track_index   = index;
    polled.play_status   = status;
    polled.volume        = level;
    polled.abs_volume    = abs_level;
    polled.mute          = device.mute;
    polled.shuffle       = shuffle;
    polled.repeat        = repeat;
    polled.trackpos_ms   = pos_ms;
    polled.trackpos_s    = pos_s;
    polled.chapter_index = chapter_index;
    polled.power_state   = power;
    polled.battery_level = battery;
    polled.hold          = hold;
    polled.numtracks     = ntracks;
}

static void iap_poll_changed_events(void)
{
    iap_poll_sample(true);
}

uint32_t iap_take_changed_events(void)
{
    uint32_t bits;

    /* If the tick has not got to it yet. This runs from the packet
     * handler, which is the iAP thread, so it is as safe a place as
     * the tick -- and doing it here as well means a poll that arrives
     * inside the first 100 ms after a reset still answers against the
     * session's own baseline rather than an empty one. */
    if (device.poll_baseline_pending) {
        device.poll_baseline_pending = false;
        iap_poll_baseline();
    }

    iap_poll_changed_events();
    bits = device.changed_notifications;
    device.changed_notifications = 0;

    return bits;
}

void iap_periodic(void)
{
#ifdef HAVE_IAP_ACCESSORY_POLL
    /* Accessory detach, on targets where the detect line costs a bus
     * transaction and so cannot be read from a tick. This has to run
     * before any of the early returns below: a detached accessory has
     * no subscriptions, which is exactly what those return on. */
    iap_accessory_poll();
#endif

    iap_apply_pending_interface_pause();

    /* Settings an accessory changed under Restore on Exit, put back now
     * that it has gone. Armed by iap_reset_lingo4() and done here
     * because the detach is noticed from a tick on the 6G, where
     * playlist_sort() and audio_flush_and_reload_tracks() are not safe.
     * Ahead of the early returns for the same reason as the poll above:
     * a detached accessory has no subscriptions. */
    iap_restore_settings();

    /* An accessory's mute, lifted now that it has gone.
     * iap_reset_device() could not do it: setvol() reaches the codec
     * and the reset runs from a tick on the 6G. */
    if (device.unmute_pending) {
        device.unmute_pending = false;
        setvol();
    }

    /* The poll baseline iap_reset_device() could not take: it runs from
     * a tick on the 6G and iap_get_trackpos() takes a mutex. */
    if (device.poll_baseline_pending) {
        device.poll_baseline_pending = false;
        iap_poll_baseline();
    }

    /* iPodModeChange, flagged by iap_record() on whatever thread
     * changed the audio input. Sent from here, where this thread owns
     * the TX buffer. */
#ifdef HAVE_LINE_REC
    iap_lingo1_send_pending();
#endif

    /* Accessory Power: high power while something is playing, low power
     * when it is not. */
    iap_high_power_track();

    /* Previous mute state, paired with device.volume for change
     * detection on the volume notification below. */

    if(!iap_setupflag) return;

    /* Handle pending authentication tasks */
    /* An accessory that answered GetAccessoryAuthenticationInfo and
     * then went quiet -- a head unit that powers its iAP MCU down while
     * leaving the connector live, a certificate transfer cut short --
     * used to park the device here for the rest of the boot. Two things
     * followed. AUST_CHASENT satisfies DEVICE_AUTHENTICATED
     * (iap-core.h), so the accessory kept every authenticated lingo
     * indefinitely with no AckAccessoryAuthenticationStatus ever sent.
     * And DEVICE_AUTH_RUNNING stayed true, so iap_task() never reached
     * its 1 Hz idle drop and the thread woke ten times a second until
     * the dock was unplugged.
     *
     * Table 3-32 (p.139) gives AckAccessoryAuthenticationStatus
     * "0x00 = Authentication operation passed, All other values =
     * Authentication operation failed", so the accessory is told rather
     * than left waiting. Device-originated here -- there is no command
     * of the accessory's to echo -- so it carries the device's own
     * transaction ID. */
    if (DEVICE_AUTH_RUNNING && device.auth.deadline
        && TIME_AFTER(current_tick, device.auth.deadline))
    {
        IAP_TX_INIT(0x00, 0x19);
        IAP_TX_PUT_IPOD_AUTH_TRANSID();
        IAP_TX_PUT(0x02);       /* Table 3-6 (p.125): command failed */
        iap_send_tx();

        /* state only, not iap_reset_auth(): that clears idps and
         * idps_started, and the accessory is still connected -- losing
         * them mid-session desynchronises every transaction ID after
         * this point. */
        device.auth.state = AUST_NONE;
        device.auth.deadline = 0;
    }

    switch (device.auth.state)
    {
        /* Both of these use the non-advancing form: MFi p.111 has
         * the Apple device not increment transaction IDs during
         * authentication, and these are the two commands it originates
         * inside the handshake. */
        case AUST_INIT:
        {
            /* Send out GetDevAuthenticationInfo */
            IAP_TX_INIT(0x00, 0x14);
            IAP_TX_PUT_IPOD_AUTH_TRANSID();

            iap_send_tx();
            device.auth.state = AUST_CERTREQ;
            /* Use Auth2's two-second limit until 0x15 supplies the version. */
            device.auth.deadline = current_tick + 2 * HZ;
            break;
        }

        case AUST_CERTDONE:
        {
            /* Send GetDevAuthenticationSignature with 20 bytes of
             * challenge and retry counter 1.  We use whatever happens
             * to be in the RX buffer as the challenge data. */
            IAP_TX_INIT(0x00, 0x17);
            IAP_TX_PUT_IPOD_AUTH_TRANSID();
            IAP_TX_PUT_DATA(iap_rxstart,
                        (device.auth.version == 0x100) ? 16 : 20);
            IAP_TX_PUT(0x01);

            iap_send_tx();
            device.auth.state = AUST_CHASENT;
            /* Auth2's class is unknown, so use its 75-second upper bound. */
            device.auth.deadline = current_tick
                + ((device.auth.version == 0x100) ? 7 * HZ : 75 * HZ);
            break;
        }

        default:
        {
            break;
        }
    }

    /* TrackNewAudioAttributes, resent until acknowledged, and sent
     * again for every new track -- Table 4-233 (p.346), lingo version
     * 1.01. The retry is bounded: an accessory that never acknowledges
     * is not helped by being asked for ever, and the packet shares the
     * TX buffer with everything else on this tick. */
    if (device.audio_attrs_pending && DEVICE_AUTHENTICATED)
    {
        device.audio_attrs_pending = false;
        audio_attrs_retries = 0;
        iap_send_audio_attrs();
    }
    else if (audio_attrs_unacked && DEVICE_AUTHENTICATED)
    {
        if (audio_attrs_retries < AUDIO_ATTRS_MAX_RETRIES) {
            audio_attrs_retries++;
            iap_send_audio_attrs();
        } else {
            audio_attrs_unacked = false;
        }
    }

#if CONFIG_TUNER
    /* GetTunerCaps, once the accessory is authenticated.
     *
     * MFi 4.7.7 (p.293), RetTunerCaps: "This command is sent by an RF
     * tuner accessory in response to a GetTunerCaps command sent by an
     * Apple device." Purely a response -- and nothing here ever sent
     * the request, so iap_handlepkt_mode7()'s case 0x02 could not run
     * on hardware at all. That case is the whole capability-driven
     * bring-up: it powers the tuner on with a status-notify mask the
     * accessory said it supports, sets the mode, and picks a band from
     * the bands the accessory advertises. All of it was reachable only
     * from the test suite, which fed RetTunerCaps by hand.
     *
     * What the driver does instead is assume: ipod_remote_tuner.c
     * powers the tuner and sets a band from Rockbox's region setting
     * without ever asking what the accessory has. That still runs, and
     * runs later -- on RADIO_REGION, when the user opens the radio --
     * so the user's region still wins, which is the right precedence.
     *
     * Deferred to the tick rather than sent at identify because lingo 7
     * requires authentication, and at identify there is none yet. */
    if (device.tuner_caps_pending && DEVICE_AUTHENTICATED)
    {
        device.tuner_caps_pending = false;

        IAP_TX_INIT(0x07, 0x01);
        IAP_TX_PUT_IPOD_TRANSID();
        iap_send_tx();
    }
#endif

    /* Digital audio activation after IDPS auth */
    if (device.audio_init_pending && DEVICE_AUTHENTICATED)
    {
        device.audio_init_pending = false;

        IAP_TX_INIT(0x0A, 0x02);
        IAP_TX_PUT_IPOD_TRANSID();
        iap_send_tx();
    }

    /* There used to be a deferred volume push here, fired once after
     * authentication for every accessory. It sent 0x03/0x0D
     * RetiPodStateInfo, which MFi 4.3.16 (p.265) defines only as the
     * response to GetiPodStateInfo; stamped it from the device's own
     * counter, so 2.6.1.1 (p.111) obliged the accessory to discard it;
     * was not gated on lingo 0x03; and used the absolute volume scale
     * where event 0x04 wants the UI one.
     *
     * The subscription path below does the job properly:
     * device.volume_reported makes it push the level once, on
     * 0x03/0x09, as soon as an accessory enables the event -- and not
     * at all to one that never does.
     */

    /* Time out button down events */
    if (iap_timeoutbtn)
        iap_timeoutbtn -= 1;

    if (!iap_timeoutbtn)
    {
        /* A seek ends when the button it holds does.
         *
         * device.pb_seeking was written in three places and cleared in
         * one -- iap_seek_stop() -- so a seek that ended any other way
         * left it set for ever, and iap_play_state_reported() went on
         * answering Fast forward to every GetPlayStatus. Three routes
         * reach here without an EndFFRew: this ten-second safety
         * release, a Simple Remote all-zero release (which MFi 4.2.7
         * (p.226) requires the accessory to send), and PlayControl
         * Stop.
         *
         * iap_seek_stop() rather than a bare assignment, so the
         * accessory is told: it is owed the seek-stop notification
         * whether or not it was the thing that ended the seek. No TX
         * buffer is open here -- this is the top of the tick. */
        if (device.pb_seeking)
            iap_seek_stop();

        iap_remotebtn = BUTTON_NONE;
        iap_repeatbtn = 0;
        iap_btnshuffle = false;
        iap_btnrepeat = false;
        iap_btnstop = false;
        iap_btnalbum = false;
        iap_btnchapter = false;
#if CONFIG_TUNER
        iap_btnradiomute = false;
#endif
    }

    /* Handle power down messages. */
    if (iap_shutdown && device.do_power_notify)
    {
        /* NotifyiPodStateChange */
        IAP_TX_INIT(0x00, 0x23);
        IAP_TX_PUT_IPOD_TRANSID();
        IAP_TX_PUT(0x01);

        iap_send_tx();

        /* No further actions, we're going down */
        iap_reset_device(&device);

            /* Spent, not latched. A shutdown can be cancelled --
             * apps/misc.c's shutdown path calls cancel_shutdown() and
             * keeps the player running when tagcache_prepare_shutdown()
             * fails -- and nothing cleared this. Power never dropped,
             * so the accessory never reidentified (3.3.29, p.143: the
             * device is "going to Hibernate state (no power)"), and
             * worse: the next accessory to identify sets
             * do_power_notify, so the very next tick told it 0x01 and
             * reset it too. No accessory could complete identification
             * for the rest of the boot. */
            iap_shutdown = false;
        return;
    }

    /* Handle GetAccessoryInfo messages */
    if (device.accinfo == ACCST_INIT)
    {
        /* GetAccessoryInfo */
        IAP_TX_INIT(0x00, 0x27);
        IAP_TX_PUT_IPOD_TRANSID();
        IAP_TX_PUT(0x00);

        iap_send_tx();
        device.accinfo = ACCST_SENT;
    }

    /* Do not send requests for device information while
     * an authentication is still running, this seems to
     * confuse some devices
     */
    if (!DEVICE_AUTH_RUNNING && (device.accinfo == ACCST_DATA))
    {
        int first_set;

        /* Find the first bit set in the capabilities field,
         * ignoring those we already asked for
         */
        first_set = find_first_set_bit(device.capabilities & (~device.capabilities_queried));

        if (first_set != 32)
        {
            /* True once something has actually gone out for this bit.
             * ACCST_SENT means "waiting for a reply", and setting it
             * without having asked anything wedges the sweep: no reply
             * can arrive, accinfo never leaves ACCST_SENT, the next bit
             * is never queried, and iap_task() never reaches the 1 Hz
             * idle drop -- 10 Hz wakeups for the rest of the
             * connection. The switch below has arms for bits 1 to 9 and
             * Table 3-48 (p.148) defines bits up to 18, of which 18,
             * "Asynchronous playback state changes", is mandatory for
             * Bluetooth accessories. */
            bool asked = false;

            /* Add bit to queried cababilities */
            device.capabilities_queried |= BIT_N(first_set);

            switch (first_set)
            {
                /* Name */
                case 0x01:
                /* Firmware version */
                case 0x04:
                /* Hardware version */
                case 0x05:
                /* Manufacturer */
                case 0x06:
                /* Model number */
                case 0x07:
                /* Serial number */
                case 0x08:
                /* Maximum payload size */
                case 0x09:
                {
                    IAP_TX_INIT(0x00, 0x27);
                    IAP_TX_PUT_IPOD_TRANSID();
                    IAP_TX_PUT(first_set);

                    iap_send_tx();
                    asked = true;
                    break;
                }

                /* Minimum supported iPod firmware version */
                case 0x02:
                {
                    IAP_TX_INIT(0x00, 0x27);
                    IAP_TX_PUT_IPOD_TRANSID();
                    IAP_TX_PUT(2);
                    IAP_TX_PUT_U32(IAP_IPOD_MODEL);
                    IAP_TX_PUT(IAP_IPOD_FIRMWARE_MAJOR);
                    IAP_TX_PUT(IAP_IPOD_FIRMWARE_MINOR);
                    asked = true;
                    IAP_TX_PUT(IAP_IPOD_FIRMWARE_REV);

                    iap_send_tx();
                    break;
                }

                /* Minimum supported lingo version. Queries Lingo 0 */
                case 0x03:
                {
                    IAP_TX_INIT(0x00, 0x27);
                    IAP_TX_PUT_IPOD_TRANSID();
                    IAP_TX_PUT(3);
                    IAP_TX_PUT(0);

                    iap_send_tx();
                    asked = true;
                    break;
                }

                default:
                    /* A capability with nothing to ask for. The bit is
                     * already marked queried, so leaving accinfo at
                     * ACCST_DATA lets the next tick move to the next
                     * one instead of waiting for a reply to a question
                     * that was never put. */
                    break;
            }

            if (asked)
                device.accinfo = ACCST_SENT;
        }
        else
        {
            /* The sweep is done. accinfo stayed at ACCST_DATA, and
             * iap_task() requires ACCST_NONE before it drops from the
             * 100 ms wakeup to the 1 s one -- so the power saving it
             * exists for was unreachable for the life of every
             * connection.
             *
             * It could never be reached by accident either: Table 3-48
             * (p.148) makes capability bits 1, 4, 5, 6 and 7
             * "Required; set to 1", so device.capabilities is always
             * non-zero and iap-lingo0.c sets ACCST_DATA on every
             * reply. Only iap_reset_device() ever set ACCST_NONE. */
            device.accinfo = ACCST_NONE;
        }
    }

    /* Notifications used to be suppressed outright once IDPS was in
     * play, because they carried no transaction IDs and would have been
     * misparsed. They carry them now, so an accessory that identified
     * through IDPS gets the state changes MFi Table 2-15 expects it to
     * receive. IAP_TX_PUT_IPOD_TRANSID() is a no-op without IDPS, so a
     * legacy accessory still sees byte-identical packets. */

    /* While USB audio source mode is streaming, keep the continuous
     * traffic off the wire but let the discrete events through.
     *
     * This used to be a blanket return, from df9c67209b ("Fix dock
     * audio glitch by suppressing unsolicited IAP..."). MFi 4.3.12
     * (p.257) does not allow that: "The Apple device sends this command
     * asynchronously whenever an enabled event change has occurred."
     * SetRemoteEventNotification was acked Success and then nothing was
     * delivered for the whole streaming session -- which for a digital
     * audio dock is the entire session, so a car head unit's display
     * never updated while music played.
     *
     * The distinction the blanket guard was missing is between events
     * that fire on a change and events that are always changing. The
     * same paragraph gives the cadence: "Notifications for enabled
     * events are sent every 500 ms, with the exception of volume change
     * notifications, which are sent every 100 ms." Elapsed track
     * position (bits 0 and 15) moves on every single one of those
     * ticks while audio plays, so it alone is a steady stream competing
     * with the isochronous transfers. A track change or a play/pause is
     * a packet every few minutes.
     *
     * The other traffic the guard was added for -- ten identical volume
     * notifications a second -- was removed by 8fc34583a6's change
     * detection and is not coming back.
     *
     * Untested against a real dock; I have no hardware. The trade is a
     * certain defect (the display never updates) against a possible one
     * (a dock disturbed by an occasional packet), and it is narrowed to
     * the events that were actually flooding. */
#ifdef USB_ENABLE_AUDIO
    const bool audio_streaming = usb_audio_source_streaming();
#else
    const bool audio_streaming = false;
#endif

    /* Once per tick, before either contents-changed test reads it. */
    if (contents_changed())
        device.contents_dirty = true;

    /* Two independent subscriptions: do_notify and device.notifications
     * are the Display Remote lingo's, pb_notifications the Extended
     * Interface's. Returning on do_notify alone meant an accessory that
     * quietened the Extended Interface with
     * SetPlayStatusChangeNotification(0) also lost every Display Remote
     * event, though MFi 4.3.11 (p.255) allows only "a remote event
     * bitmask of 0x0" or a detach to do that. */
    if (!device.do_notify && device.pb_notifications == 0) return;
    if ((device.notifications == 0) && (interface_state != IST_EXTENDED)
        && device.pb_notifications == 0) return;

    /* Volume, checked every 100ms rather than the 500ms the other
     * events use. MFi 4.3.12 (p.256) says the notification goes out
     * "whenever an enabled event change has occurred", and that volume
     * changes are the one thing polled at 100ms; the cadence is how
     * often we look, not a heartbeat.
     *
     * This used to transmit unconditionally on every tick, so a car
     * head unit that enabled bit 4 received ten identical volume
     * notifications a second for as long as it stayed connected. That
     * is the sort of traffic the usb_audio_source_streaming() guard
     * above was added to keep off the wire.
     *
     * device.volume holds the transmitted byte, not the codec's dB
     * value. It is an unsigned char and the dB value is signed, so
     * storing the latter turned -25 dB into 231 and made any
     * comparison meaningless.
     */
    if (device.notifications & (BIT_N(4) | BIT_N(16))) {
        /* Event 0x04 is "Mute/UI Volume" and event 0x10's byte 1 is the
         * UI volume, so both are the limit-normalized scale. Only event
         * 0x10's byte 2 is the absolute one. */
        unsigned char level = device.mute
                            ? 0 : iap_volume_to_ui_byte(global_status.volume);
        unsigned char abs_level = device.mute
                            ? 0 : iap_volume_to_byte(global_status.volume);

        if (!device.volume_reported
            || device.volume != level || device.mute != periodic_last_mute) {
            device.volume_reported = true;
            device.volume = level;
            periodic_last_mute = device.mute;

            /* Bit 16 selects the three-byte Mute/UI/Absolute form
             * (Table 4-61 event 0x10); bit 4 the two-byte Mute/UI one. */
            /* Table 4-59 (p.255) lists bit 4 "Mute/UI Volume" and bit
             * 16 "Mute/UI/Absolute Volume" as separate events, and
             * 4.3.11 says "Notification for each event can be enabled
             * by setting the associated bit". An accessory that set
             * both must receive both; an else here silently withheld
             * event 0x04 from it. */
            if (device.notifications & BIT_N(4)) {
                IAP_TX_INIT(0x03, 0x09);
                IAP_TX_PUT_IPOD_TRANSID();
                IAP_TX_PUT(0x04);
                IAP_TX_PUT(device.mute ? 0x01 : 0x00);
                IAP_TX_PUT(level);
                device.changed_notifications |= BIT_N(4);
                iap_send_tx();
            }

            if (device.notifications & BIT_N(16)) {
                IAP_TX_INIT(0x03, 0x09);
                IAP_TX_PUT_IPOD_TRANSID();
                IAP_TX_PUT(0x10);
                IAP_TX_PUT(device.mute ? 0x01 : 0x00);
                IAP_TX_PUT(level);          /* UI, normalized to the limit */
                IAP_TX_PUT(abs_level);      /* absolute, not normalized */
                device.changed_notifications |= BIT_N(16);
                iap_send_tx();
            }
        }
    }

    /* All other events are sent every 500ms */
    periodic_count += 1;
    /* The track change iap_track_changed() flagged from the audio
     * thread. Sent from here, where this thread owns the TX buffer.
     *
     * iap_get_trackindex(), not playlist_next(0), for two reasons.
     *
     * playlist_next() blocks, and this thread does not in fact own the
     * buffer across a blocking call: its first statement is
     * dc_thread_stop(), which under HAVE_DIRCACHE -- on for both
     * targets -- is queue_send(&playlist_queue, PLAYLIST_DC_SCAN_STOP)
     * and so an unconditional block_thread()/switch_thread(), not mere
     * lock contention. With the buffer open at IAP_TX_INIT4 above, that
     * lets the UI thread in, and on the 5G a tuner_set() from the radio
     * screen reaches iap_send_pkt(), which rewinds iap_txnext.
     *
     * And it is a mutating call used as a query: it assigns
     * playlist->index, and on its index < 0 path re-shuffles the queue
     * or calls create_and_play_dir(). iap_get_trackindex() is three
     * plain field reads, is what the Display Remote track-index
     * notification below already uses, and returns the same value here
     * -- calculate_step_count(playlist, 0) is 0 for any current track
     * that is not PLAYLIST_SKIPPED, and a track change means one just
     * started. */
    if ((interface_state == IST_EXTENDED)
        && device.pb_track_changed
        && (device.pb_notifications & BIT_N(2))) {
        IAP_TX_INIT4(0x04, 0x0027);
        IAP_TX_PUT_IPOD_TRANSID();
        IAP_TX_PUT(0x01);
        IAP_TX_PUT_U32(iap_get_trackindex());
        iap_send_tx();
    }
    device.pb_track_changed = false;

    if (periodic_count < 5) return;

    periodic_count = 0;

    struct iap_chapter_info chapter;
    bool chapter_valid = false;
    if (((interface_state == IST_EXTENDED)
         && (device.pb_notifications
             & (BIT_N(5) | BIT_N(6) | BIT_N(7))))
        || (device.notifications & BIT_N(2)))
    {
        chapter_valid = (audio_status() & AUDIO_STATUS_PLAY)
                     && iap_current_chapter(audio_current_track(), &chapter);
    }

    /* RemoteEventNotification */

    /* Mode 04 PlayStatusChangeNotification, Table 5-47 (p.426) type
     * 0x04 "Track time offset (ms)" -- Table 5-45 (p.425) bit 03.
     *
     * This fired on every tick for any accessory in Extended Interface
     * mode, whatever it had subscribed to and whether or not the
     * position had moved: two packets a second at a standstill. Every
     * Display Remote event below is change-detected; this one was not. */
    /* Table 5-45 (p.425) bit 00: "Basic play state changes (stop, FFW
     * seek stop, or REW seek stop, using status notification types
     * 0x00, 0x02, or 0x03)", and Table 5-47 (p.426) gives type 0x00 as
     * "Playback stopped {0x00}".
     *
     * Nothing sent it. The only two senders of 0x0027 in the tree were
     * types 0x01 and 0x04, so an accessory that subscribed to bit 00 --
     * including via the one-byte 0x01 form, which this file maps onto
     * bits 0, 2, 3 and 5 -- was acked Success and then never told
     * playback had stopped. */
    if ((interface_state == IST_EXTENDED)
        && (device.pb_notifications & BIT_N(0))) {
        unsigned char pb_status = audio_status();

        if (device.pb_play_status != pb_status) {
            bool was_playing = (device.pb_play_status &
                                (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE)) != 0;
            bool stopped_now = (pb_status &
                                (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE)) == 0;

            device.pb_play_status = pb_status;

            if (was_playing && stopped_now) {
                IAP_TX_INIT4(0x04, 0x0027);
                IAP_TX_PUT_IPOD_TRANSID();
                /* Type 0x00 and nothing else. Table 5-47 (p.426) gives
                 * it as "Playback stopped {0x00}" -- no parameter,
                 * where 0x01 is "{0x01, trackIndex:4}" and does have
                 * one. The second byte here was borrowed from type
                 * 0x0B's playMode, and MFi 2.5.2 (p.110) has the length
                 * field count "the lengths of all command parameters
                 * being passed", so the packet went out declaring a
                 * parameter this command does not define. */
                IAP_TX_PUT(0x00);       /* Playback stopped */
                iap_send_tx();
            }
        }
    }

    /* Table 5-45 (p.425) bit 01, the extended play state. The stop
     * above is bit 00's; this one also carries playing and paused,
     * which nothing else does. Seek start and stop are sent from the
     * PlayControl arms in iap-lingo4.c, where the seek is known. */
    if ((interface_state == IST_EXTENDED)
        && (device.pb_notifications & BIT_N(1))) {
        unsigned char st = iap_play_state_byte();

        if (st != device.pb_ext_state) {
            device.pb_ext_state = st;
            /* Table 5-47 (p.427): 0x02 Stopped, 0x0A Playing,
             * 0x0B Paused. */
            iap_send_pb_extended(st == 0x00 ? 0x02 :
                                 st == 0x01 ? 0x0A : 0x0B);
        }
    }

    /* Suppressed while streaming, for the reason the Display Remote
     * position events below are: this is the same per-tick clock, on a
     * different lingo. 8c63099d8e narrowed the blanket guard and missed
     * this one. */
    if ((interface_state == IST_EXTENDED)
        && !audio_streaming
        && (device.pb_notifications & BIT_N(3))) {
        /* Return Track Position */
        struct mp3entry *id3 = audio_current_track();
        unsigned long time_elapsed = id3->elapsed;

        if (time_elapsed != device.pb_trackpos_ms) {
            device.pb_trackpos_ms = time_elapsed;

            IAP_TX_INIT4(0x04, 0x0027);
            IAP_TX_PUT_IPOD_TRANSID();
            IAP_TX_PUT(0x04);
            IAP_TX_PUT_U32(time_elapsed);

            iap_send_tx();
        }
    }

    /* Table 5-45 (p.425) bit 04, "Track time offset (sec)" -- Table
     * 5-47 (p.427) type 0x07, {0x07, trackOffsetSec:4}. The seconds
     * counterpart of the milliseconds above, and the one an accessory
     * that only draws a clock wants: it subscribed, was acked Success
     * and heard nothing. The Display Remote lingo has served its
     * equivalent, bit 15, all along. */
    if ((interface_state == IST_EXTENDED)
        && !audio_streaming
        && (device.pb_notifications & BIT_N(4))) {
        uint32_t secs = iap_get_trackpos() / 1000;

        if (secs != device.pb_trackpos_s) {
            device.pb_trackpos_s = secs;

            IAP_TX_INIT4(0x04, 0x0027);
            IAP_TX_PUT_IPOD_TRANSID();
            IAP_TX_PUT(0x07);
            IAP_TX_PUT_U32(secs);

            iap_send_tx();
        }
    }

    if ((interface_state == IST_EXTENDED)
        && (device.pb_notifications & BIT_N(5))) {
        uint32_t index = chapter_valid ? chapter.index : UINT32_MAX;

        if (index != device.pb_chapter_index) {
            device.pb_chapter_index = index;
            IAP_TX_INIT4(0x04, 0x0027);
            IAP_TX_PUT_IPOD_TRANSID();
            IAP_TX_PUT(0x05);
            IAP_TX_PUT_U32(index);
            iap_send_tx();
        }
    }

    if ((interface_state == IST_EXTENDED) && !audio_streaming
        && chapter_valid && (device.pb_notifications & BIT_N(6))
        && chapter.elapsed_ms != device.pb_chapterpos_ms) {
        device.pb_chapterpos_ms = chapter.elapsed_ms;
        IAP_TX_INIT4(0x04, 0x0027);
        IAP_TX_PUT_IPOD_TRANSID();
        IAP_TX_PUT(0x08);
        IAP_TX_PUT_U32(chapter.elapsed_ms);
        iap_send_tx();
    }

    if ((interface_state == IST_EXTENDED) && !audio_streaming
        && chapter_valid && (device.pb_notifications & BIT_N(7))) {
        uint32_t secs = chapter.elapsed_ms / 1000;

        if (secs != device.pb_chapterpos_s) {
            device.pb_chapterpos_s = secs;
            IAP_TX_INIT4(0x04, 0x0027);
            IAP_TX_PUT_IPOD_TRANSID();
            IAP_TX_PUT(0x09);
            IAP_TX_PUT_U32(secs);
            iap_send_tx();
        }
    }

    /* Table 5-45 bit 12, "Playback engine contents changed" -- type
     * 0x0E, {0x0E, numTracks:4}. A head unit that keeps its own list
     * has no other way to learn the queue was rebuilt, and MFi p.59
     * says so: "Continuous polling, using GetPlayStatus, is not an
     * acceptable alternative." */
    if ((interface_state == IST_EXTENDED)
        && (device.pb_notifications & BIT_N(12))) {
        uint32_t n = (uint32_t)playlist_amount();

        if (n != device.pb_numtracks || device.contents_dirty) {
            device.pb_numtracks = n;

            IAP_TX_INIT4(0x04, 0x0027);
            IAP_TX_PUT_IPOD_TRANSID();
            IAP_TX_PUT(0x0E);
            IAP_TX_PUT_U32(n);

            iap_send_tx();
        }
    }

    /* Track position (ms)  or Track position (s) */
    if (!audio_streaming && (device.notifications & (BIT_N(0) | BIT_N(15))))
    {
        uint32_t t;
        uint16_t ts;
        /* Both assignments below are conditional, so a track whose
         * position has not moved used to read this uninitialised.
         * Confirmed by UBSan: "load of value 199, which is not a valid
         * value for type 'bool'". */
        bool changed = false;

        t = iap_get_trackpos();
        ts = (t / 1000) & 0xFFFF;

        if ((device.notifications & BIT_N(0)) && (device.trackpos_ms != t))
        {
            IAP_TX_INIT(0x03, 0x09);
            IAP_TX_PUT_IPOD_TRANSID();
            IAP_TX_PUT(0x00);
            IAP_TX_PUT_U32(t);
            device.changed_notifications |= BIT_N(0);
            changed = true;

            iap_send_tx();
        }

        if ((device.notifications & BIT_N(15)) && (device.trackpos_s != ts)) {
            IAP_TX_INIT(0x03, 0x09);
            IAP_TX_PUT_IPOD_TRANSID();
            IAP_TX_PUT(0x0F);
            IAP_TX_PUT_U16(ts);
            device.changed_notifications |= BIT_N(15);
            changed = true;

            iap_send_tx();
        }

        if (changed)
        {
            device.trackpos_ms = t;
            device.trackpos_s = ts;
        }
    }

    /* Track index */
    if (device.notifications & BIT_N(1))
    {
        uint32_t index;

        index = iap_get_trackindex();

        if (device.track_index != index) {
            IAP_TX_INIT(0x03, 0x09);
            IAP_TX_PUT_IPOD_TRANSID();
            IAP_TX_PUT(0x01);
            IAP_TX_PUT_U32(index);
            device.changed_notifications |= BIT_N(1);

            iap_send_tx();

            device.track_index = index;
        }
    }

    if (device.notifications & BIT_N(2))
    {
        uint32_t index = chapter_valid ? chapter.index : UINT32_MAX;
        uint32_t track = iap_get_trackindex();
        uint16_t count = chapter_valid ? chapter.count : 0;

        if (device.chapter_index != index
            || device.chapter_count != count
            || ((count != 0 || device.chapter_count != 0)
                && device.chapter_track_index != track))
        {
            IAP_TX_INIT(0x03, 0x09);
            IAP_TX_PUT_IPOD_TRANSID();
            IAP_TX_PUT(0x02);
            IAP_TX_PUT_U32(track);
            IAP_TX_PUT_U16(count);
            IAP_TX_PUT_U16(index);
            device.changed_notifications |= BIT_N(2);

            iap_send_tx();

            device.chapter_index = index;
            device.chapter_track_index = track;
            device.chapter_count = count;
        }
    }

    /* Play status */
    if (device.notifications & BIT_N(3))
    {
        unsigned char play_status;

        /* The value that goes out, not audio_status(). The two differ
         * across a seek: Table 4-63 (p.262) has play status 0x03 Fast
         * forward and 0x04 Rewind, iap_play_state_reported() answers
         * them from device.pb_seeking, and audio_status() stays
         * AUDIO_STATUS_PLAY throughout. So the state an accessory was
         * told went 0x01 -> 0x03 -> 0x01 with no notification for
         * either edge, on the one event where a head unit most needs
         * to redraw. */
        play_status = iap_play_state_reported();
        if (device.play_status != play_status)
        {
            IAP_TX_INIT(0x03, 0x09);
            IAP_TX_PUT_IPOD_TRANSID();
            IAP_TX_PUT(0x03);
            IAP_TX_PUT(play_status);
            device.changed_notifications |= BIT_N(3);

            iap_send_tx();

            device.play_status = play_status;

            /* This used to call audio_pause() or audio_resume() and
             * mute or unmute the tuner from here. A notification is a
             * report -- MFi 4.3.12 (p.257) gives
             * RemoteEventNotification "Origin: Apple device" -- and
             * nothing about announcing a state change authorises
             * driving the thing that changed.
             *
             * It was also a loop. Opening the FM radio screen calls
             * audio_stop() (apps/radio/radio.c), which lands here as a
             * play-status change: the accessory was sent "Stopped",
             * then tuner_set(RADIO_MUTE, 1) made
             * ipod_remote_tuner.c send its own "Paused" on the same
             * event -- two contradictory packets -- and put the tuner
             * hardware to sleep, which is the radio the user had just
             * opened.
             *
             * The audio calls were no-ops in every consistent state
             * anyway: play_status is what the player had just
             * reported. */
        }
    }

    /* Power/Battery */
    if (device.notifications & BIT_N(5))
    {
        unsigned char power_state;
        unsigned char battery_l;

        power_state = charger_input_state;
        battery_l = battery_level();

        if (!device.power_reported
            || (device.power_state != power_state)
            || (device.battery_level != battery_l))
        {
            device.power_reported = true;
            IAP_TX_INIT(0x03, 0x09);
            IAP_TX_PUT_IPOD_TRANSID();
            IAP_TX_PUT(0x05);

            iap_fill_power_state();
            device.changed_notifications |= BIT_N(5);

            iap_send_tx();

            device.power_state = power_state;
            device.battery_level = battery_l;
        }
    }

    /* Equalizer state
     * This is not handled yet.
     *
     * TODO: Fix equalizer handling
     */

    /* Shuffle */
    if (device.notifications & BIT_N(7))
    {
        unsigned char shuffle;

        shuffle = global_settings.playlist_shuffle;

        if (device.shuffle != shuffle)
        {
            IAP_TX_INIT(0x03, 0x09);
            IAP_TX_PUT_IPOD_TRANSID();
            IAP_TX_PUT(0x07);
            IAP_TX_PUT(shuffle?0x01:0x00);
            device.changed_notifications |= BIT_N(7);

            iap_send_tx();

            device.shuffle = shuffle;
        }
    }

    /* Repeat */
    if (device.notifications & BIT_N(8))
    {
        unsigned char repeat;

        repeat = global_settings.repeat_mode;

        if (device.repeat != repeat)
        {
            IAP_TX_INIT(0x03, 0x09);
            IAP_TX_PUT_IPOD_TRANSID();
            IAP_TX_PUT(0x08);
            switch (repeat)
            {
                case REPEAT_OFF:
                {
                    IAP_TX_PUT(0x00);
                    break;
                }

                case REPEAT_ONE:
                {
                    IAP_TX_PUT(0x01);
                    break;
                }

                case REPEAT_ALL:
                {
                    IAP_TX_PUT(0x02);
                    break;
                }

                default:
                {
                    /* REPEAT_SHUFFLE and REPEAT_AB are Rockbox modes
                     * with no encoding in Table 4-64 (p.262), which
                     * defines 0x00 off, 0x01 one, 0x02 all and calls
                     * 0x03-0xFF reserved. Without an arm here no data
                     * byte was appended at all: event 0x08 has "Data
                     * length in bytes 0x01" (Table 4-61, p.259), so the
                     * packet went out a byte short and the accessory
                     * read the checksum as the repeat state. Reachable
                     * with no accessory involvement -- the user picks
                     * Repeat: Shuffle in Rockbox's own settings.
                     *
                     * Both continue over the whole queue, so "all" is
                     * the closest thing the table has. iap-lingo4.c
                     * already answers 0x02 for them. */
                    IAP_TX_PUT(0x02);
                    break;
                }
            }
            device.changed_notifications |= BIT_N(8);

            iap_send_tx();

            device.repeat = repeat;
        }
    }

    /* Date/Time */
    if (device.notifications & BIT_N(9))
    {
        struct tm* tm;

        tm = get_time();

        /* The five fields that go out, not the whole struct.
         *
         * struct tm also carries tm_sec, tm_wday, tm_yday and
         * tm_isdst, and get_time() (firmware/common/timefuncs.c)
         * re-reads the RTC once a second -- so tm_sec advanced, the
         * memcmp differed, and this sent a byte-identical event 0x09
         * fifty-nine more times a minute, for the life of the session.
         * Table 4-61 (p.260) defines the payload as year, month, day,
         * hour and minute; MFi 4.3.12 (p.257) has the device send
         * "whenever an enabled event change has occurred", and a second
         * passing is not one of them. Same class as the ten identical
         * volume notifications a second this file eliminated earlier,
         * one order of magnitude smaller. */
        if (tm->tm_year != device.datetime.tm_year
            || tm->tm_mon  != device.datetime.tm_mon
            || tm->tm_mday != device.datetime.tm_mday
            || tm->tm_hour != device.datetime.tm_hour
            || tm->tm_min  != device.datetime.tm_min)
        {
            IAP_TX_INIT(0x03, 0x09);
            IAP_TX_PUT_IPOD_TRANSID();
            IAP_TX_PUT(0x09);
            /* MFi Table 4-61 (p.260) and Table 4-72 (p.264):
            * "A value of 2005 represents the year 2005 A.D."
            * Rockbox keeps tm_year as years since 1900, the
            * POSIX convention -- rtc-6g.c:47 and
            * rtc_pcf50605.c:54 both compute "buf[6] + 100",
            * and valid_time() rejects anything outside
            * 100..199 -- so it needs converting. Sent raw it
            * put year 126 on the wire in 2026. */
            IAP_TX_PUT_U16(tm->tm_year + 1900);

            /* Month */
            IAP_TX_PUT(tm->tm_mon+1);

            /* Day */
            IAP_TX_PUT(tm->tm_mday);

            /* Hour */
            IAP_TX_PUT(tm->tm_hour);

            /* Minute */
            IAP_TX_PUT(tm->tm_min);

            device.changed_notifications |= BIT_N(9);

            iap_send_tx();

            memcpy(&(device.datetime), tm, sizeof(struct tm));
        }
    }

    /* Alarm
     * This is not supported yet.
     *
     * TODO: Fix alarm handling
     */

    /* Backlight
     * This is not supported yet.
     *
     * TODO: Fix backlight handling
     */

    /* Hold switch */
    if (device.notifications & BIT_N(0x0C))
    {
        unsigned char hold;

        hold = button_hold();
        if (device.hold != hold) {
            IAP_TX_INIT(0x03, 0x09);
            IAP_TX_PUT_IPOD_TRANSID();
            IAP_TX_PUT(0x0C);
            IAP_TX_PUT(hold?0x01:0x00);

            device.changed_notifications |= BIT_N(0x0C);

            iap_send_tx();

            device.hold = hold;
        }
    }

    /* Playback engine contents, Table 4-59 (p.256) bit 18, event 0x12
     * in Table 4-61 (p.261): four bytes, "number of tracks in new
     * playlist".
     *
     * The accounting comment in iap-lingo3.c grouped this bit with five
     * others and called them all stubs -- "equalizer_index, backlight,
     * soundcheck and audiobook are assigned constants above and nothing
     * ever moves them". Four names for six bits, and this was one of
     * the two it did not name: its value is playlist_amount(), which
     * moves whenever the queue is rebuilt, and iap_periodic() already
     * tracks and sends exactly it for the Extended Interface twin
     * (Table 5-45 bit 12) a few hundred lines up.
     *
     * MFi 1.11.2.1.2 (p.59) is why it matters: "an accessory that
     * displays a list of tracks currently being played must respond to
     * every 'Playback engine contents changed' notification...
     * Continuous polling, using GetPlayStatus, is not an acceptable
     * alternative." A head unit that never enters Extended Interface
     * mode subscribed, was acked Success, and kept a stale list for the
     * rest of the session. */
    if (device.notifications & BIT_N(18))
    {
        uint32_t n = (uint32_t)playlist_amount();

        if (device.numtracks != n || device.contents_dirty) {
            IAP_TX_INIT(0x03, 0x09);
            IAP_TX_PUT_IPOD_TRANSID();
            IAP_TX_PUT(0x12);
            IAP_TX_PUT_U32(n);

            device.changed_notifications |= BIT_N(18);

            iap_send_tx();

            device.numtracks = n;
        }
    }

    /* Both contents-changed events have had their look at it. */
    device.contents_dirty = false;

    /* Sound check
     * This is not supported yet.
     *
     * TODO: Fix sound check handling
     */

    /* Audiobook check
     * This is not supported yet.
     *
     * TODO: Fix audiobook handling
     */
}

/* Change the current interface state.
 * On a change from IST_EXTENDED to IST_STANDARD, or from IST_STANDARD
 * to IST_EXTENDED, pause playback, if playing
 */
void iap_interface_state_change(const enum interface_state new)
{
    int state_irq = disable_irq_save();

    if (interface_state == new) {
        restore_irq(state_irq);
        return;
    }

    interface_pause_pending = false;
    interface_state = new;
    restore_irq(state_irq);
    if ((audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
        == AUDIO_STATUS_PLAY)
    {
        /* A synthetic Play button can activate the focused screen instead
         * of pausing. */
        audio_pause();
    }

    if (interface_state == new)
        button_queue_post(new == IST_EXTENDED ? SYS_IAP_UI_ENTER
                                              : SYS_IAP_UI_EXIT, 0);
}

bool iap_remote_ui_active(void)
{
    return interface_state == IST_EXTENDED;
}

static void iap_handlepkt_mode5(const unsigned int len, const unsigned char *buf)
{
    /* Nothing to do, and nothing to say.
     *
     * MFi Table C-37 (p.548) gives the Accessory Power lingo exactly two
     * commands, BeginHighPower (0x02) and EndHighPower (0x03), and C.8.2
     * and C.8.3 both give them "Lingo: 0x05 - Origin: Apple device".
     * 0x00-0x01 and 0x04-0xFF are Reserved. No accessory-originated
     * command exists, so nothing should arrive here at all, and the
     * lingo has no acknowledgement in either direction.
     *
     * This used to echo the command straight back: receiving 0x05 0x02
     * made the device transmit 0x05 0x02, which is BeginHighPower sent
     * to the accessory in reply to something the accessory should never
     * have sent. It read buf[1] after discarding len, so a two-byte
     * frame produced that from whatever byte followed in the RX buffer.
     *
     * The device's own outbound BeginHighPower, sent from the Identify
     * and IdentifyDeviceLingoes paths in iap-lingo0.c, is the correct
     * direction and is untouched.
     */
    (void)len;
    (void)buf;
}

/* Digital Audio lingo (0x0A) handler */
static void iap_handlepkt_mode10(const unsigned int len, const unsigned char *buf)
{
    unsigned int cmd = buf[1];
    int off = 2;

    /* Two bytes for the lingo and the command, before anything else. */
    if (len < 2)
        return;

    /* Skip the accessory's transaction ID when present. Nothing this
     * lingo sends is a response, so the ID is never echoed back: the
     * one command we originate here carries our own counter instead.
     *
     * The test was device.auth.idps, which is only set at EndIDPS --
     * inside the IDPS window it is still false while the accessory is
     * already sending IDs. DEVICE_TRANSID_ACTIVE covers both, and is
     * what every other lingo uses; iap-core.h documents the narrower
     * test as a mistake this tree has shipped twice.
     *
     * len >= 4, not > 4: under IDPS a command with no parameters is
     * exactly four bytes -- lingo, command, and the two-byte ID -- so
     * "> 4" left the ID unread for every one of them and the rejection
     * below quoted 0x0000. MFi 2.6.1.1 (p.111) then obliges the
     * accessory to discard the rejection. Command 0x18 in
     * iap-lingo0.c carries a comment about the same off-by-one. */
    /* The transaction ID a rejection has to quote. Captured here so
     * both rejections below quote the same thing, and quote 0x0000 when
     * the packet was too short to carry one -- which is the honest
     * answer, and the only one that keeps the reply the right length.
     * Emitting nothing under IDPS made the accessory read the status
     * byte as the ID's high byte, which the accessory model in the test
     * suite caught the moment the second rejection was added. */
    unsigned char l10_tid_hi = 0, l10_tid_lo = 0;

    if (DEVICE_TRANSID_ACTIVE && len >= 4) {
        off = 4;
        l10_tid_hi = buf[2];
        l10_tid_lo = buf[3];
    }

    /* MFi 4.10.1 (p.345): "Every accessory that supports the Digital
     * Audio lingo must authenticate itself with a connected Apple
     * device as soon as the Apple device recognizes the accessory;
     * deferred authentication is not permitted." So an unauthenticated
     * accessory has no business here, and neither has one that never
     * negotiated the lingo -- every other handler checks both. */
    if (!DEVICE_LINGO_SUPPORTED(0x0A) || !DEVICE_AUTHENTICATED)
        return;

    switch (cmd)
    {
        /* AccessoryAck (0x00). Table 4-234 (p.353): command status,
         * then "The ID for the command being acknowledged". This is
         * what stops TrackNewAudioAttributes being resent -- lingo
         * version 1.01, Table 4-233 (p.346). Ignoring it meant the
         * resend had nothing to stop it, which is why there was no
         * resend. */
        case 0x00:
            if (len >= (unsigned int)(off + 2) && buf[off + 1] == 0x04)
                audio_attrs_unacked = false;
            break;

        /* RetAccSampleRateCaps (0x03) — respond with TrackNewAudioAttributes */
        case 0x03:
        {
            /* Table 4-237 (p.355) makes the payload "a list of n sample
             * rates (32-bit big-endian format)", so a conformant packet
             * carries at least one. 4.10.8 adds that every accessory
             * must support 32 kHz, 44.1 kHz and 48 kHz, so in practice
             * n is at least three. Without this the reply went out for
             * a packet carrying no rates at all. */
            if (len < (unsigned int)off + 4)
                break;

#ifdef USB_ENABLE_AUDIO
            unsigned long frequency;
#endif
            /* The accessory's own list, which MFi 4.10.9 (p.356) says
             * the answer must come from: "The sample rate sent to the
             * accessory is taken from the list of sample rates returned
             * to the Apple device by the RetAccessorySampleRateCaps
             * command. If the accessory supports the sample rate of the
             * current audio track, then it is sent as the current
             * sample rate. If the accessory does not support the sample
             * rate, the Apple device resamples the audio data to a
             * supported sample rate in real time and sends this new
             * supported sample rate as the current sample rate."
             *
             * The list used to be discarded and the mixer's own rate
             * reported whatever it was. This hardware runs at anything
             * from 8 to 48 kHz (HW_SAMPR_CAPS, ipod6g.h:34), and an
             * accessory is only obliged to support three of those, so a
             * 22.05 kHz track told the dock to expect a rate it may not
             * have. */
            unsigned int nrates = (len - off) / 4;
            unsigned int r;
            bool has_32k = false, has_44k = false, has_48k = false;

            for (r = 0; r < nrates; r++) {
                uint32_t rate = get_u32(&buf[off + r * 4]);
                if (rate == 32000)  has_32k = true;
                if (rate == 44100)  has_44k = true;
                if (rate == 48000)  has_48k = true;
            }

            /* MFi 4.10.8 (p.355): "At a minimum, every accessory must
             * support the sample rates 32 KHz, 44.1 KHz, and 48 KHz. A
             * RetAccessorySampleRateCaps command with sample rates not
             * listed in Table 4-238, or missing any of the required
             * sample rates, is invalid. If the Apple device receives
             * such a command, it sends the accessory an iPodAck command
             * with a negative acknowledgment as the command status." */
            if (!has_32k || !has_44k || !has_48k) {
                /* Table 4-232 (p.346) lists two acknowledgements on this
                 * lingo: 0x00 AccessoryAck, Origin: Accessory, and 0x01
                 * iPodAck, Origin: Apple device. This used to answer on
                 * the General lingo, which has no field able to name a
                 * lingo 0x0A command -- so the accessory read it as a
                 * rejection of General command 0x03,
                 * RequestExtendedInterfaceMode, which it never sent. */
                IAP_TX_INIT(0x0A, 0x01);
                /* A rejection is a response, so it echoes the id of the
                 * command it answers rather than taking one from the
                 * device's counter -- MFi 2.6.1.1 (p.111) has the
                 * accessory discard anything whose id matches no
                 * command it sent. */
                if (DEVICE_TRANSID_ACTIVE) {
                    IAP_TX_PUT(l10_tid_hi);
                    IAP_TX_PUT(l10_tid_lo);
                }
                IAP_TX_PUT(IAP_ACK_BAD_PARAM);
                IAP_TX_PUT(0x03);
                iap_send_tx();
                break;
            }
            /* TrackNewAudioAttributes is a command the Apple device
             * originates, not a response: MFi 4.10.9 (p.356) gives it
             * "Origin: Apple device" and has the accessory acknowledge
             * it with AccessoryAck. It therefore carries an ID from our
             * own counter.
             *
             * It used to echo the ID from the accessory's
             * RetAccessorySampleRateCaps, which is the ID we already
             * spent on GetAccessorySampleRateCaps. Reusing a completed
             * transaction's ID leaves the accessory free to treat this
             * as a duplicate, and leaves our counter able to hand the
             * same value out again later. */
#ifdef USB_ENABLE_AUDIO
            frequency = iap_audio_pending_frequency ?
                        iap_audio_pending_frequency : mixer_get_frequency();

            /* Only report a rate the accessory said it can take. If the
             * one we would have sent is not in its list, move the mixer
             * to 44.1 kHz -- which 4.10.8 obliges every accessory to
             * support, and which the check above has just confirmed is
             * in this one's list -- so that what goes down the wire and
             * what we tell the accessory to expect are the same thing.
             * That is the resample 4.10.9 describes. */
            {
                bool supported = false;
                for (r = 0; r < nrates; r++)
                    if (get_u32(&buf[off + r * 4]) == frequency)
                        supported = true;

                if (!supported) {
                    frequency = 44100;
                    mixer_set_frequency(frequency);
                }
            }

            usb_audio_set_source_sampling_frequency(frequency);
            iap_audio_reported_frequency = frequency;
            iap_audio_pending_frequency = 0;
#endif
            /* Through the shared emitter, so this send is tracked and
             * resent until acknowledged like every other one. Built
             * inline here, it was not -- which is what made the resend
             * added for lingo version 1.01 fire for nothing. */
            audio_attrs_retries = 0;
            iap_send_audio_attrs();
            break;
        }

        default:
            /* MFi 4.10.6 (p.354): "The Apple device sends the iPodAck
             * command when it receives an invalid or unsupported
             * command or a bad parameter." Table 4-232 (p.346) reserves
             * 0x06-0xFF, and 0x02, 0x04 and 0x05 are Origin: Apple
             * device, so nothing but 0x00 and 0x03 should arrive here.
             *
             * This lingo was the only one that answered an unsupported
             * command with silence. Every other one rejects, and an
             * accessory waiting on a reply that never comes has to time
             * out to discover that -- MFi 2.6.1.1 (p.111) gives it no
             * other way, since there is nothing to match against.
             *
             * A rejection is a response, so it echoes the transaction
             * ID of the command it answers rather than taking one from
             * the device's counter. */
            IAP_TX_INIT(0x0A, 0x01);
            if (DEVICE_TRANSID_ACTIVE) {
                IAP_TX_PUT(l10_tid_hi);
                IAP_TX_PUT(l10_tid_lo);
            }
            IAP_TX_PUT(IAP_ACK_BAD_PARAM);
            IAP_TX_PUT(cmd);
            iap_send_tx();
            break;
    }
}

/* Throw away a packet that started arriving and never finished.
 *
 * MFi Accessory Hardware Specification R9, Table 3-2 (p.56), link
 * control bit 0: "0 indicates that this HID report is the first in a
 * set of one or more reports. This also implies that any previous sets
 * are completed. Any incomplete iAP packets received prior to the
 * arrival of this report are flushed and lost."
 *
 * The transport can abandon a report set -- a set that never closes, or
 * an opener with no start marker -- but it had no way to say so, so the
 * framer stayed mid-packet in ST_DATA and swallowed the next command's
 * bytes as continuation data. That command was lost too, and with a
 * long enough abandoned length so was the one after it.
 *
 * Returning to ST_SYNC is the whole of the fix. The buffer looks after
 * itself: ST_SYNC already does "iap_rxnext = iap_rxpayload" when a sync
 * byte arrives (see iap_getc()), rolling back to the end of the last
 * complete packet, and the transport injects one at the head of every
 * frame. What stopped that happening was the state -- in ST_DATA the
 * 0xFF is payload, not a sync byte, so the rollback was never reached.
 *
 * An earlier version of this function rolled the pointer back here as
 * well. That was redundant, and tracing the dispatcher with it removed
 * showed every packet still arriving well formed.
 */
/* Throw away everything the accessory has said, complete or not.
 *
 * iap_rx_flush() below resets the framer only: a packet that had
 * arrived whole and was waiting for iap_handlepkt() survived it, and on
 * detach that is a packet from an accessory that is gone. Draining one
 * buffered Identify after iap_reset_state() put the lingoes, the
 * authentication state, radio_present and the power-notify flag all
 * back -- the session re-established itself from a corpse, and the next
 * accessory inherited it.
 *
 * IRQ-safe, which it has to be: serial_acc_tick() calls
 * iap_reset_state() from a tick on the 6G. Pointer writes under
 * disable_irq_save(), the same way iap_handlepkt()'s resync does it. */
static void iap_rx_discard_all(void)
{
    int level = disable_irq_save();

    frame_state.state = ST_SYNC;
    frame_state.len   = 0;
    frame_state.check = 0;
    frame_state.count = 0;

    iap_rxnext    = iap_rxstart;
    iap_rxpayload = iap_rxstart;
    iap_rxlen     = RX_BUFLEN + 2;

    restore_irq(level);
}

void iap_rx_flush(void)
{
    int level = disable_irq_save();

    frame_state.state = ST_SYNC;
    frame_state.len   = 0;
    frame_state.check = 0;
    frame_state.count = 0;

    restore_irq(level);
}

void iap_handlepkt(void)
{
    int level;
    int length;

    if(!iap_setupflag) return;
    if(!iap_running) return;

    /* The number of queued IAP_EV_MSG_RCVD events does not reliably
     * match the number of complete packets in the RX buffer, since the
     * event queue can overflow during input bursts. Drain every
     * complete packet instead, they occupy the buffer from iap_rxstart
     * up to iap_rxpayload.
     */
    while (iap_rxpayload > iap_rxstart)
    {
        /* if we are waiting for a remote button to go out,
           delay the handling of the new packet */
        if(iap_repeatbtn)
        {
            queue_post(&iap_queue, IAP_EV_MSG_RCVD, 0);
            sleep(1);
            return;
        }

        /* handle command by mode */
        length = get_u16(iap_rxstart);
#if defined(LOGF_ENABLE) && defined(ROCKBOX_HAS_LOGF)
        logf("R: %s", hexstring(iap_rxstart+2, (length)));
#endif

        /* A corrupt length would walk iap_rxpayload past the start of
         * the buffer and turn the bounded memmove below into a huge
         * copy, so stop draining and resynchronise instead.
         */
        if ((length + 2) > (iap_rxpayload - iap_rxstart))
        {
            level = disable_irq_save();
            iap_rxnext = iap_rxstart;
            iap_rxpayload = iap_rxstart;
            iap_rxlen = RX_BUFLEN+2;
            restore_irq(level);
            return;
        }

        if (length != 0) {
            unsigned char mode = *(iap_rxstart+2);
            switch (mode) {
            case 0: iap_handlepkt_mode0(length, iap_rxstart+2); break;
#ifdef HAVE_LINE_REC
            case 1: iap_handlepkt_mode1(length, iap_rxstart+2); break;
#endif
            case 2: iap_handlepkt_mode2(length, iap_rxstart+2); break;
            case 3: iap_handlepkt_mode3(length, iap_rxstart+2); break;
            case 4: iap_handlepkt_mode4(length, iap_rxstart+2); break;
            case 5: iap_handlepkt_mode5(length, iap_rxstart+2); break;
#if CONFIG_TUNER
            case 7: iap_handlepkt_mode7(length, iap_rxstart+2); break;
#endif
            case 10: iap_handlepkt_mode10(length, iap_rxstart+2); break;
            }
        }

        /* Remove the handled packet from the RX buffer
         * This needs to be done with interrupts disabled, to make
         * sure the buffer and the pointers into it are handled
         * cleanly
         */
        level = disable_irq_save();
        {
            ptrdiff_t remaining =
                (iap_rxnext - iap_rxstart) - (ptrdiff_t)(length + 2);
            if (remaining < 0)
            {
                /* iap_reset_buffers() ran while a handler above was
                 * yielding -- iap_malloc() is called from the USB
                 * thread on HID transport activation and only guards
                 * on iap_running, so it can rewind these pointers
                 * under us. The bytes still buffered belong to the new
                 * session, so drop them rather than hand memmove a
                 * negative length, which as size_t is a ~4GB copy.
                 */
                iap_rxnext = iap_rxstart;
                iap_rxpayload = iap_rxstart;
                iap_rxlen = RX_BUFLEN+2;
                restore_irq(level);
                return;
            }
            memmove(iap_rxstart, iap_rxstart + (length + 2),
                    (size_t)remaining);
        }
        iap_rxnext -= (length+2);
        iap_rxpayload -= (length+2);
        iap_rxlen += (length+2);
        restore_irq(level);

        /* poke the poweroff timer */
        reset_poweroff_timer();
    }
    /* iap_timeoutbtn is decremented only by iap_periodic(), which runs
     * on the tick -- and iap_task() drops that tick to 1 Hz once the
     * link is idle. A button arriving between two of those posts
     * IAP_EV_MSG_RCVD, not IAP_EV_TICK, so the auto-release waited on a
     * timer already armed for a second. MFi 4.2.4 (p.218) puts the
     * release at "approximately 200 ms after the last button status
     * command".
     *
     * Overshooting it matters. firmware/drivers/button.c crosses
     * REPEAT_START at 300 ms, so a lost release turns a tap into a
     * seek; and iap_btnshuffle and iap_btnrepeat stay latched until the
     * counter drains, swallowing the next Shuffle or Repeat tap.
     *
     * Re-arm at 100 ms so the counter starts draining now. */
    if (iap_timeoutbtn && iap_running)
        timeout_register(&iap_task_tmo, iap_task, MS_TO_TICKS(100), 0);

}

int remote_control_rx(void)
{
    int btn;
    int level = disable_irq_save();

    /* Bracketed because this runs on the button driver's thread --
     * button-clickwheel.c ORs the result into the physical read -- and
     * the decrement is a read-modify-write against two other writers.
     * iap-lingo2.c and iap_seek_start() assign iap_repeatbtn = 2 from
     * the iAP thread, and iap_reset_device() zeroes it from a tick. A
     * lost update drops a packet deferral one packet early, which is
     * the mechanism that exists so a raised button reaches this
     * function before the next command is handled. */
    btn = iap_remotebtn;
    if(iap_repeatbtn)
        iap_repeatbtn--;

    restore_irq(level);
    return btn;
}

const unsigned char *iap_get_serbuf(void)
{
    return iap_rxstart;
}

#ifdef IAP_MALLOC_DYNAMIC
/* Not built: IAP_MALLOC_DYNAMIC is defined in no config header, and
 * both targets take the static serbuf. Documented rather than deleted
 * because the buffer is the one thing here that would have to move if
 * it ever were, and this callback is wrong three ways for whoever
 * turns it on:
 *
 *   - it resets iap_txnext to the payload start, so a compaction
 *     between IAP_TX_INIT and iap_send_tx() silently truncates a
 *     half-built packet -- and it discards `current`, which is the one
 *     value needed to preserve the offset instead;
 *   - it recomputes iap_rxstart from iap_buffers, which nothing here
 *     updates, so the RX base still points into the old allocation;
 *   - iap_rxnext and iap_rxpayload are not rebased at all, and none of
 *     it is bracketed against iap_getc() writing them from the UART
 *     interrupt.
 */
static int iap_move_callback(int handle, void* current, void* new)
{
    (void) handle;
    (void) current;

    iap_txstart = new;
    iap_txpayload = iap_txstart+5;
    iap_txnext = iap_txpayload;
    iap_rxstart = iap_buffers+(TX_BUFLEN+6);

    return BUFLIB_CB_OK;
}
#endif

/* Change the shuffle state */
/* Restore on Exit, MFi Table 4-75 (p.270): "0x00 Do not save the
 * original state. 0x01 Save the original state and restore it on exit."
 *
 * Both lingoes offer it. Extended Interface has it on SetShuffle and
 * SetRepeat (Tables 5-55 and 5-60), and Display Remote on
 * SetiPodStateInfo info types 0x04, 0x07, 0x08 and 0x10 (Table 4-74,
 * pp.267-269). It lived in iap-lingo4.c, which is why the Display
 * Remote types ignored the byte entirely and wrote the user's settings
 * to config.cfg whatever the accessory asked for.
 *
 * It belongs with the setting it guards, so it is here, and both
 * lingoes reach it through the same calls.
 */
static bool shuffle_restore;
static bool shuffle_original;
static bool shuffle_restore_pending;
static bool repeat_restore;
static int  repeat_original;
static bool repeat_restore_pending;
static bool volume_restore;
static int  volume_original;
static bool volume_restore_pending;

/* The volume half, for SetiPodStateInfo types 0x04 and 0x10. Both carry
 * a bRestoreOnExit byte and neither read it, so a dock that turned the
 * volume up for its own speakers and asked for the level back left the
 * user's iPod loud -- the one restore-on-exit setting a user notices
 * immediately, and the one this tree had no latch for.
 *
 * Only the level is remembered. Mute is not a setting that survives the
 * accessory: iap_reset_device() lifts it unconditionally, which is what
 * device.unmute_pending is for. */
void iap_volume_restore_arm(const enum iap_restore restore)
{
    if (restore == IAP_RESTORE_YES && !volume_restore)
    {
        volume_original = global_status.volume;
        volume_restore = true;
    }
    else if (restore == IAP_RESTORE_NO)
        volume_restore = false;
}

void iap_shuffle_state(const bool state, const enum iap_restore restore)
{
    if (restore == IAP_RESTORE_YES && !shuffle_restore)
    {
        shuffle_original = global_settings.playlist_shuffle;
        shuffle_restore = true;
    }
    else if (restore == IAP_RESTORE_NO)
        shuffle_restore = false;

    /* Set shuffle to enabled */
    if(state && !global_settings.playlist_shuffle)
    {
        global_settings.playlist_shuffle = 1;
        /* Not saved when the accessory asked for it back: what is on
         * disk is still the user's own setting. */
        if (!shuffle_restore)
            settings_save();
        if (audio_status() & AUDIO_STATUS_PLAY)
            playlist_randomise(NULL, current_tick, true);
    }
    /* Set shuffle to disabled */
    else if(!state && global_settings.playlist_shuffle)
    {
        global_settings.playlist_shuffle = 0;
        if (!shuffle_restore)
            settings_save();
        if (audio_status() & AUDIO_STATUS_PLAY)
            playlist_sort(NULL, true);
    }
}

/* Change the repeat state */
void iap_repeat_state(const unsigned char state, const enum iap_restore restore)
{
    if (restore == IAP_RESTORE_YES && !repeat_restore)
    {
        repeat_original = global_settings.repeat_mode;
        repeat_restore = true;
    }
    else if (restore == IAP_RESTORE_NO)
        repeat_restore = false;

    if (state != global_settings.repeat_mode)
    {
        global_settings.repeat_mode = state;
        if (!repeat_restore)
            settings_save();
        if (audio_status() & AUDIO_STATUS_PLAY)
            audio_flush_and_reload_tracks();
    }
}

/* Arm the restore. Called from iap_reset_device(), which runs from a
 * tick on the 6G -- putting the playlist back is not safe there, so
 * this only flags it. */
void iap_arm_settings_restore(void)
{
    /* One flag each. Collapsing them into a single pending bit and then
     * clearing both latches destroyed the only record of which had been
     * armed, and iap_restore_settings() then put *both* back from
     * statics that are zero when unarmed -- so an accessory that asked
     * for its shuffle change to be undone also reset the user's repeat
     * mode to off, and one that asked for repeat back wiped shuffle.
     * A single SetShuffle with no state change and RestoreOnExit set
     * was enough to destroy a user's Repeat All. */
    shuffle_restore_pending = shuffle_restore;
    repeat_restore_pending = repeat_restore;
    volume_restore_pending = volume_restore;
    shuffle_restore = false;
    repeat_restore = false;
    volume_restore = false;
}

/* Called from iap_periodic(), in thread context. */
void iap_restore_settings(void)
{
    if (!shuffle_restore_pending && !repeat_restore_pending
        && !volume_restore_pending)
        return;

    /* No settings_save() on the way back: the temporary change was
     * never saved, so what is on disk is already the original. */
    if (shuffle_restore_pending
        && shuffle_original != (bool)global_settings.playlist_shuffle)
    {
        global_settings.playlist_shuffle = shuffle_original;
        if (audio_status() & AUDIO_STATUS_PLAY)
        {
            if (shuffle_original)
                playlist_randomise(NULL, current_tick, true);
            else
                playlist_sort(NULL, true);
        }
    }

    if (repeat_restore_pending
        && repeat_original != global_settings.repeat_mode)
    {
        global_settings.repeat_mode = repeat_original;
        if (audio_status() & AUDIO_STATUS_PLAY)
            audio_flush_and_reload_tracks();
    }

    /* setvol() rather than a bare assignment, for the same reason the
     * accessory's own change went through it: it is what reaches the
     * codec. Ordered after the two playlist settings because those can
     * reload the buffer, and the level should be right before sound
     * comes back. */
    if (volume_restore_pending && volume_original != global_status.volume)
    {
        global_status.volume = volume_original;
        setvol();
    }

    shuffle_restore_pending = false;
    repeat_restore_pending = false;
    volume_restore_pending = false;
}

void iap_repeat_next(void)
{
    switch (global_settings.repeat_mode)
    {
        case REPEAT_OFF:
        {
            iap_repeat_state(REPEAT_ALL, IAP_RESTORE_KEEP);
            break;
        }
        case REPEAT_ALL:
        {
            iap_repeat_state(REPEAT_ONE, IAP_RESTORE_KEEP);
            break;
        }
        case REPEAT_ONE:
        {
            iap_repeat_state(REPEAT_OFF, IAP_RESTORE_KEEP);
            break;
        }
        default:
        {
            /* REPEAT_SHUFFLE and REPEAT_AB are outside the cycle this
             * walks, so the Simple Remote Repeat button was a permanent
             * no-op once the user had selected either from Rockbox's
             * own settings. Rejoin at the start of the cycle. */
            iap_repeat_state(REPEAT_OFF, IAP_RESTORE_KEEP);
            break;
        }
    }
}

/* This function puts the current power/battery state
 * into the TX buffer. The buffer is assumed to be initialized
 */
void iap_fill_power_state(void)
{
    unsigned char power_state;
    unsigned char battery_l;

    power_state = charger_input_state;
    battery_l = battery_level();

    /* MFi Table 4-65 (p.263): "0x00 Internal battery power, low power
     * (< 30%), 0x01 Internal battery power, 0x02 External power,
     * battery pack, no charging, 0x03 External power, no charging,
     * 0x04 External power, battery charging, 0x05 External power,
     * battery charged."
     *
     * Two things were wrong. CHARGER_UNPLUGGED counted as external
     * power -- powermgmt.h calls it a "Transitional state during
     * CHARGER=>NO_CHARGER", so the accessory saw a charging icon for
     * the instant after the charger came out. And 0x05 was never sent:
     * a full battery on the dock read as still charging, for as long as
     * it stayed there.
     *
     * That fix did not work, and the test that covered it passed by
     * building states the hardware cannot produce.
     * charging_algorithm_step() (firmware/powermgmt.c:593) is the
     * CHARGING_MONITOR arm both targets select, and it sets
     * charge_state to CHARGING for exactly CHARGER_PLUGGED and CHARGER
     * and to DISCHARGING for exactly CHARGER_UNPLUGGED and NO_CHARGER.
     * The first branch here already took the latter two, so
     * "charge_state == DISCHARGING" below it was unreachable -- and
     * unreachable twice over, because battery_level()
     * (firmware/powermgmt.c:365) clamps to 99 whenever charge_state is
     * above DISCHARGING, so battery_l >= 100 cannot hold on external
     * power either. A docked iPod reported 0x04 for the whole session,
     * including at full charge, which is the defect the paragraph
     * above claims to have cured.
     *
     * charging_state() is the signal that actually answers the
     * question: one GPIO bit per target reading the charger IC
     * (power-6g.c:233, power-ipod.c:82), not a variable derived from
     * charger_input_state. With it off while external power is
     * present, 99 is what the clamp makes "as full as this reads", so
     * that is 0x05 and anything lower is 0x03.
     *
     * The battery byte is zero on external power, which is what three
     * places in the spec require: Table 4-61 (p.259) event 0x05, "if
     * an external power status is returned, the battery level is
     * invalid and is returned as 0"; 4.3.30 (p.279); and Table 4-91
     * (p.279), "this field is valid only if powerStat is Internal
     * battery". It was sent as a level on the strength of a comment
     * citing Table 4-64 for it -- that table is Repeat state, on
     * p.262, and says nothing about batteries. */
    if (power_state == NO_CHARGER || power_state == CHARGER_UNPLUGGED) {
        IAP_TX_PUT(battery_l < 30 ? 0x00 : 0x01);
        IAP_TX_PUT((unsigned char)((battery_l * 255) / 100));
        return;
    }

#if CONFIG_CHARGING
    if (charging_state())
        IAP_TX_PUT(0x04);
    else
        IAP_TX_PUT(battery_l >= 99 ? 0x05 : 0x03);
#else
    IAP_TX_PUT(0x04);
#endif
    IAP_TX_PUT(0x00);
}

#include "lcd.h"
#include "font.h"
bool dbg_iap(void)
{
    lcd_setfont(FONT_SYSFIXED);

    while (1)
    {
        if (action_userabort(HZ/10))
            break;

        lcd_clear_display();

        /* show internal state of IAP subsystem */
        lcd_putsf(0, 0, "auth: %d acc: %d", device.auth.state, device.accinfo);
        lcd_putsf(0, 1, "lin: %08x", device.lingoes);
        lcd_putsf(0, 2, "notif: %08x", device.notifications);
        lcd_putsf(0, 3, "cap: %08x/%08x", device.capabilities, device.capabilities_queried);

        // frame_state.state
        // serial state

        lcd_update();
    }

    lcd_setfont(FONT_UI);
    return false;
}
