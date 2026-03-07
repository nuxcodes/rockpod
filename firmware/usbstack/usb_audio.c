/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id:  $
 *
 * Copyright (C) 2010 by Amaury Pouly
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* NOTE
 *
 * This is USBAudio 1.0. USBAudio 2.0 is notably _not backwards compatible!_
 * USBAudio 1.0 over _USB_ 2.0 is perfectly valid!
 *
 * Relevant specifications are USB 2.0 and USB Audio Class 1.0.
 */

#include "string.h"
#include "system.h"
#include "usb_core.h"
#include "usb_drv.h"
#include "kernel.h"
#include "sound.h"
#include "usb_class_driver.h"
#include "usb_audio_def.h"
#include "pcm_sampr.h"
#include "audio.h"
#include "sound.h"
#include "stdlib.h"
#include "fixedpoint.h"
#include "misc.h"
#include "settings.h"
#include "core_alloc.h"
#include "pcm_mixer.h"
#include "dsp_core.h"
#ifdef HAVE_CS42L55
#include "audiohw.h"
#endif

/* #define LOGF_ENABLE */
#include "logf.h"

/* Fixed-point conversion macros (signed Q16.16) */
#define TO_16DOT16_FIXEDPT(val) ((int32_t)(val) * (1<<16))
#define TO_DOUBLE(val) ((double)(val) / (1<<16))

/* Audio Control Interface */
static struct usb_interface_descriptor
    ac_interface =
{
    .bLength            = sizeof(struct usb_interface_descriptor),
    .bDescriptorType    = USB_DT_INTERFACE,
    .bInterfaceNumber   = 0,
    .bAlternateSetting  = 0,
    .bNumEndpoints      = 0,
    .bInterfaceClass    = USB_CLASS_AUDIO,
    .bInterfaceSubClass = USB_SUBCLASS_AUDIO_CONTROL,
    .bInterfaceProtocol = 0,
    .iInterface         = 0
};

/* Audio Control Terminals/Units */
/* Use concrete struct since we need space for 2 interface numbers
 * (playback sink AS + source AS) */
static struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bDescriptorSubType;
    uint16_t bcdADC;
    uint16_t wTotalLength;
    uint8_t  bInCollection;
    uint8_t  baInterfaceNr[2];
} __attribute__ ((packed, unused)) ac_header =
{
    .bLength            = USB_AC_SIZEOF_HEADER(2), /* two streaming interfaces */
    .bDescriptorType    = USB_DT_CS_INTERFACE,
    .bDescriptorSubType = USB_AC_HEADER,
    .bcdADC             = 0x0100, /* Identifies this as usb audio class 1.0 */
    .wTotalLength       = 0, /* fill later */
    .bInCollection      = 2, /* two streaming interfaces */
    .baInterfaceNr      = {0, 0}, /* fill later */
};

enum
{
    /* Source IDs first (1, 2) — matches Apple/ipod-gadget layout */
    AC_SOURCE_INPUT_TERMINAL_ID = 1,
    AC_SOURCE_OUTPUT_TERMINAL_ID,
    /* Playback IDs follow (3, 4, 5) — only used in sink mode */
    AC_PLAYBACK_INPUT_TERMINAL_ID,
    AC_PLAYBACK_FEATURE_ID,
    AC_PLAYBACK_OUTPUT_TERMINAL_ID,
};

static struct usb_ac_input_terminal __attribute__((unused)) ac_playback_input =
{
    .bLength            = sizeof(struct usb_ac_input_terminal),
    .bDescriptorType    = USB_DT_CS_INTERFACE,
    .bDescriptorSubType = USB_AC_INPUT_TERMINAL,
    .bTerminalId        = AC_PLAYBACK_INPUT_TERMINAL_ID,
    .wTerminalType      = USB_AC_TERMINAL_STREAMING,
    .bAssocTerminal     = 0,
    .bNrChannels        = 2,
    .wChannelConfig     = USB_AC_CHANNELS_LEFT_RIGHT_FRONT,
    .iChannelNames      = 0,
    .iTerminal          = 0,
};

static struct usb_ac_output_terminal __attribute__((unused)) ac_playback_output =
{
    .bLength            = sizeof(struct usb_ac_output_terminal),
    .bDescriptorType    = USB_DT_CS_INTERFACE,
    .bDescriptorSubType = USB_AC_OUTPUT_TERMINAL,
    .bTerminalId        = AC_PLAYBACK_OUTPUT_TERMINAL_ID,
    .wTerminalType      = USB_AC_OUTPUT_TERMINAL_HEADPHONES,
    .bAssocTerminal     = 0,
    .bSourceId          = AC_PLAYBACK_FEATURE_ID,
    .iTerminal          = 0,
};

/* Feature Unit with 2 logical channels and 1 byte(8 bits) per control */
DEFINE_USB_AC_FEATURE_UNIT(8, 2)

static struct usb_ac_feature_unit_8_2 __attribute__((unused)) ac_playback_feature =
{
    .bLength            = sizeof(struct usb_ac_feature_unit_8_2),
    .bDescriptorType    = USB_DT_CS_INTERFACE,
    .bDescriptorSubType = USB_AC_FEATURE_UNIT,
    .bUnitId            = AC_PLAYBACK_FEATURE_ID,
    .bSourceId          = AC_PLAYBACK_INPUT_TERMINAL_ID,
    .bControlSize       = 1, /* by definition */
    .bmaControls        = {
        [0] = USB_AC_FU_MUTE | USB_AC_FU_VOLUME,
        [1] = 0,
        [2] = 0
    },
    .iFeature = 0
};

/* Source direction: audio FROM the iPod TO the USB host (for external DACs) */
static struct usb_ac_input_terminal ac_source_input =
{
    .bLength            = sizeof(struct usb_ac_input_terminal),
    .bDescriptorType    = USB_DT_CS_INTERFACE,
    .bDescriptorSubType = USB_AC_INPUT_TERMINAL,
    .bTerminalId        = AC_SOURCE_INPUT_TERMINAL_ID,
    .wTerminalType      = USB_AC_INPUT_TERMINAL_MICROPHONE, /* matches Apple layout */
    .bAssocTerminal     = AC_SOURCE_OUTPUT_TERMINAL_ID,
    .bNrChannels        = 2,
    .wChannelConfig     = USB_AC_CHANNELS_LEFT_RIGHT_FRONT,
    .iChannelNames      = 0,
    .iTerminal          = 0,
};

static struct usb_ac_output_terminal ac_source_output =
{
    .bLength            = sizeof(struct usb_ac_output_terminal),
    .bDescriptorType    = USB_DT_CS_INTERFACE,
    .bDescriptorSubType = USB_AC_OUTPUT_TERMINAL,
    .bTerminalId        = AC_SOURCE_OUTPUT_TERMINAL_ID,
    .wTerminalType      = USB_AC_TERMINAL_STREAMING, /* to USB host */
    .bAssocTerminal     = AC_SOURCE_INPUT_TERMINAL_ID,
    .bSourceId          = AC_SOURCE_INPUT_TERMINAL_ID, /* direct: no feature unit */
    .iTerminal          = 0,
};

/* Source-only AC header for config 2 (matches Apple layout: bInCollection=1) */
static struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bDescriptorSubType;
    uint16_t bcdADC;
    uint16_t wTotalLength;
    uint8_t  bInCollection;
    uint8_t  baInterfaceNr[1];
} __attribute__ ((packed)) ac_source_header =
{
    .bLength            = USB_AC_SIZEOF_HEADER(1),
    .bDescriptorType    = USB_DT_CS_INTERFACE,
    .bDescriptorSubType = USB_AC_HEADER,
    .bcdADC             = 0x0100,
    .wTotalLength       = 0, /* fill later */
    .bInCollection      = 1, /* one streaming interface (source only) */
    .baInterfaceNr      = {0}, /* fill later */
};

/* Audio Streaming Interface */
/* Alternative: no streaming */
static struct usb_interface_descriptor __attribute__((unused))
    as_interface_alt_idle_playback =
{
    .bLength            = sizeof(struct usb_interface_descriptor),
    .bDescriptorType    = USB_DT_INTERFACE,
    .bInterfaceNumber   = 0,
    .bAlternateSetting  = 0,
    .bNumEndpoints      = 0,
    .bInterfaceClass    = USB_CLASS_AUDIO,
    .bInterfaceSubClass = USB_SUBCLASS_AUDIO_STREAMING,
    .bInterfaceProtocol = 0,
    .iInterface         = 0
};

/* Alternative: output streaming */
static struct usb_interface_descriptor __attribute__((unused))
    as_interface_alt_playback =
{
    .bLength            = sizeof(struct usb_interface_descriptor),
    .bDescriptorType    = USB_DT_INTERFACE,
    .bInterfaceNumber   = 0,
    .bAlternateSetting  = 1,
    .bNumEndpoints      = 2, /* iso audio, iso feedback */
    .bInterfaceClass    = USB_CLASS_AUDIO,
    .bInterfaceSubClass = USB_SUBCLASS_AUDIO_STREAMING,
    .bInterfaceProtocol = 0,
    .iInterface         = 0
};

/* Class Specific Audio Streaming Interface */
static struct usb_as_interface __attribute__((unused))
    as_playback_cs_interface =
{
    .bLength            = sizeof(struct usb_as_interface),
    .bDescriptorType    = USB_DT_CS_INTERFACE,
    .bDescriptorSubType = USB_AS_GENERAL,
    .bTerminalLink      = AC_PLAYBACK_INPUT_TERMINAL_ID,
    .bDelay             = 1,
    .wFormatTag         = USB_AS_FORMAT_TYPE_I_PCM
};

static struct usb_as_format_type_i_discrete
    as_playback_format_type_i =
{
    .bLength            = USB_AS_SIZEOF_FORMAT_TYPE_I_DISCRETE((HW_FREQ_44+1)),
    .bDescriptorType    = USB_DT_CS_INTERFACE,
    .bDescriptorSubType = USB_AS_FORMAT_TYPE,
    .bFormatType        = USB_AS_FORMAT_TYPE_I,
    .bNrChannels        = 2, /* Stereo */
    .bSubframeSize      = 2, /* 2 bytes per sample */
    .bBitResolution     = 16, /* all 16-bits are used */
    .bSamFreqType       = (HW_FREQ_44+1),
    .tSamFreq           = {
        /* only values 44.1k and higher (array is in descending order) */
        [0 ... HW_FREQ_44 ] = {0}, /* filled later */
    }
};

static struct usb_as_iso_audio_endpoint __attribute__((unused))
    as_iso_audio_out_ep =
{
    .bLength          = sizeof(struct usb_as_iso_audio_endpoint),
    .bDescriptorType  = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_OUT, /* filled later */
    .bmAttributes     = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ASYNC | USB_ENDPOINT_USAGE_DATA,
    .wMaxPacketSize   = 0, /* filled later */
    .bInterval        = 0, /* filled later - 1 for full speed, 4 for high-speed */
    .bRefresh         = 0,
    .bSynchAddress    = 0 /* filled later to the address of as_iso_synch_in_ep */
};

/*
 * Updaing the desired sample frequency:
 *
 * The iso OUT ep is inextricably linked to the feedback iso IN ep
 * when using Asynchronous mode. It periodically describes to the host
 * how fast to send the data.
 *
 * Some notes from the usbaudio 1.0 documentation:
 * - bSyncAddress of the iso OUT ep must be set to the address of the iso IN feedback ep
 * - bSyncAddress of the iso IN feedback ep must be zero
 * - F_f (desired sampling frequency) describes directly the number of samples the endpoint
 *     wants to receive per frame to match the actual sampling frequency F_s
 * - There is a value, (2^(10-P)), which is how often (in 1mS frames) the F_f value will be sent
 * - P appears to be somewhat arbitrary, though the spec wants it to relate the real sample rate
 *     F_s to the master clock rate F_m by the relationship (F_m = F_s * (2^(P-1)))
 * - The above description of P is somewhat moot because of how much buffering we have. I suspect it
 *     was written for devices with essentially zero buffering.
 * - bRefresh of the feedback endpoint descriptor should be set to (10-P). This can range from 1 to 9.
 *     A value of 1 would mean refreshing every 2^1 mS = 2 mS, a value of 9 would mean refreshing every
 *     2^9 mS = 512 mS.
 * - The F_f value should be encoded in "10.10" format, but justified to the leftmost 24 bits,
 *     so it ends up looking like "10.14" format. This format is 3 bytes long. On USB 2.0, it seems that
 *     the USB spec overrides the UAC 1.0 spec here, so high-speed bus operation needs "16.16" format,
 *     in a 4 byte packet.
 */
#define FEEDBACK_UPDATE_RATE_P 5
#define FEEDBACK_UPDATE_RATE_REFRESH (10-FEEDBACK_UPDATE_RATE_P)
#define FEEDBACK_UPDATE_RATE_FRAMES (0x1<<FEEDBACK_UPDATE_RATE_REFRESH)
static struct usb_as_iso_synch_endpoint __attribute__((unused))
    as_iso_synch_in_ep =
{
    .bLength          = sizeof(struct usb_as_iso_synch_endpoint),
    .bDescriptorType  = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_IN, /* filled later */
    .bmAttributes     = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_NONE | USB_ENDPOINT_USAGE_FEEDBACK,
    .wMaxPacketSize   = 4,
    .bInterval        = 0, /* filled later - 1 or 4 depending on bus speed */
    .bRefresh         = FEEDBACK_UPDATE_RATE_REFRESH, /* This describes how often this ep will update F_f (see above) */
    .bSynchAddress    = 0  /* MUST be zero! */
};

static struct usb_as_iso_ctrldata_endpoint __attribute__((unused))
    as_iso_ctrldata_samfreq =
{
    .bLength            = sizeof(struct usb_as_iso_ctrldata_endpoint),
    .bDescriptorType    = USB_DT_CS_ENDPOINT,
    .bDescriptorSubType = USB_AS_EP_GENERAL,
    .bmAttributes       = USB_AS_EP_CS_SAMPLING_FREQ_CTL,
    .bLockDelayUnits    = 0, /* undefined */
    .wLockDelay         = 0 /* undefined */
};

/* ===== Source Audio Streaming Interface (iPod -> USB host) ===== */

/* Alternative: no streaming */
static struct usb_interface_descriptor
    as_interface_alt_idle_source =
{
    .bLength            = sizeof(struct usb_interface_descriptor),
    .bDescriptorType    = USB_DT_INTERFACE,
    .bInterfaceNumber   = 0, /* filled later */
    .bAlternateSetting  = 0,
    .bNumEndpoints      = 0,
    .bInterfaceClass    = USB_CLASS_AUDIO,
    .bInterfaceSubClass = USB_SUBCLASS_AUDIO_STREAMING,
    .bInterfaceProtocol = 0,
    .iInterface         = 0
};

/* Alternative: input streaming (one ISO IN data endpoint) */
static struct usb_interface_descriptor
    as_interface_alt_source =
{
    .bLength            = sizeof(struct usb_interface_descriptor),
    .bDescriptorType    = USB_DT_INTERFACE,
    .bInterfaceNumber   = 0, /* filled later */
    .bAlternateSetting  = 1,
    .bNumEndpoints      = 1, /* iso audio IN only */
    .bInterfaceClass    = USB_CLASS_AUDIO,
    .bInterfaceSubClass = USB_SUBCLASS_AUDIO_STREAMING,
    .bInterfaceProtocol = 0,
    .iInterface         = 0
};

/* Class Specific Audio Streaming Interface (source) */
static struct usb_as_interface
    as_source_cs_interface =
{
    .bLength            = sizeof(struct usb_as_interface),
    .bDescriptorType    = USB_DT_CS_INTERFACE,
    .bDescriptorSubType = USB_AS_GENERAL,
    .bTerminalLink      = AC_SOURCE_OUTPUT_TERMINAL_ID,
    .bDelay             = 1,
    .wFormatTag         = USB_AS_FORMAT_TYPE_I_PCM
};

static struct usb_as_format_type_i_discrete
    as_source_format_type_i =
{
    .bLength            = USB_AS_SIZEOF_FORMAT_TYPE_I_DISCRETE(HW_NUM_FREQ),
    .bDescriptorType    = USB_DT_CS_INTERFACE,
    .bDescriptorSubType = USB_AS_FORMAT_TYPE,
    .bFormatType        = USB_AS_FORMAT_TYPE_I,
    .bNrChannels        = 2, /* Stereo */
    .bSubframeSize      = 2, /* 2 bytes per sample */
    .bBitResolution     = 16,
    .bSamFreqType       = HW_NUM_FREQ,
    .tSamFreq           = {
        [0 ... HW_NUM_FREQ - 1] = {0}, /* filled later */
    }
};

/* ISO IN data endpoint for source audio (no sync — matches Apple layout) */
static struct usb_as_iso_audio_endpoint
    as_iso_audio_in_ep =
{
    .bLength          = sizeof(struct usb_as_iso_audio_endpoint),
    .bDescriptorType  = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_IN, /* filled later */
    .bmAttributes     = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_NONE | USB_ENDPOINT_USAGE_DATA,
    .wMaxPacketSize   = 192, /* 48000 Hz * 2ch * 2bytes / 1000 */
    .bInterval        = 0, /* filled later */
    .bRefresh         = 0,
    .bSynchAddress    = 0
};

/* Class-Specific AS ISO Data Endpoint for source */
static struct usb_as_iso_ctrldata_endpoint
    as_iso_ctrldata_source_samfreq =
{
    .bLength            = sizeof(struct usb_as_iso_ctrldata_endpoint),
    .bDescriptorType    = USB_DT_CS_ENDPOINT,
    .bDescriptorSubType = USB_AS_EP_GENERAL,
    .bmAttributes       = USB_AS_EP_CS_SAMPLING_FREQ_CTL,
    .bLockDelayUnits    = 0,
    .wLockDelay         = 0
};

/* Source-only AC class-specific descriptors (for config 2 wTotalLength) */
static const struct usb_descriptor_header* const ac_source_cs_descriptors_list[] =
{
    (struct usb_descriptor_header *) &ac_source_header,
    (struct usb_descriptor_header *) &ac_source_input,
    (struct usb_descriptor_header *) &ac_source_output,
};

#define AC_SOURCE_CS_DESCRIPTORS_LIST_SIZE \
    (sizeof(ac_source_cs_descriptors_list)/sizeof(ac_source_cs_descriptors_list[0]))

/* Source-only descriptors for config 2 (matches Apple/ipod-gadget layout):
 * AC interface + source terminals + source AS + iAP HID (separate driver) */
static const struct usb_descriptor_header* const usb_source_descriptors_list[] =
{
    /* Audio Control — source only */
    (struct usb_descriptor_header *) &ac_interface,
    (struct usb_descriptor_header *) &ac_source_header,
    (struct usb_descriptor_header *) &ac_source_input,
    (struct usb_descriptor_header *) &ac_source_output,
    /* Audio Streaming - Source (iPod -> host) */
    /*   Idle Source */
    (struct usb_descriptor_header *) &as_interface_alt_idle_source,
    /*   Source */
    (struct usb_descriptor_header *) &as_interface_alt_source,
    (struct usb_descriptor_header *) &as_source_cs_interface,
    (struct usb_descriptor_header *) &as_source_format_type_i,
    (struct usb_descriptor_header *) &as_iso_audio_in_ep,
    (struct usb_descriptor_header *) &as_iso_ctrldata_source_samfreq,
};

#define USB_SOURCE_DESCRIPTORS_LIST_SIZE \
    (sizeof(usb_source_descriptors_list)/sizeof(usb_source_descriptors_list[0]))

static int usb_interface; /* first interface */
static int usb_as_playback_intf_alt; /* playback streaming interface alternate setting */
static int usb_as_source_intf_alt; /* source streaming interface alternate setting */

static int as_playback_freq_idx; /* audio playback streaming frequency index (in hw_freq_sampr) */
static int as_source_freq_idx; /* audio source streaming frequency index (in hw_freq_sampr) */

struct usb_class_driver_ep_allocation usb_audio_ep_allocs[3] = {
    /* output isochronous endpoint (sink: host -> iPod) — optional in source-only config 2 */
    {.type = USB_ENDPOINT_XFER_ISOC, .dir = DIR_OUT, .optional = true},
    /* input feedback isochronous endpoint (for sink async mode) — optional in source-only config 2 */
    {.type = USB_ENDPOINT_XFER_ISOC, .dir = DIR_IN, .optional = true},
    /* input isochronous endpoint (source: iPod -> host) */
    {.type = USB_ENDPOINT_XFER_ISOC, .dir = DIR_IN, .optional = false},
};

#define EP_ISO_OUT (usb_audio_ep_allocs[0].ep)
#define EP_ISO_FEEDBACK_IN (usb_audio_ep_allocs[1].ep)
#define EP_ISO_SOURCE_IN (usb_audio_ep_allocs[2].ep)

/* small buffer used for control transfers */
static unsigned char usb_buffer[128] USB_DEVBSS_ATTR;

/* number of buffers: 2 is double-buffering (one for usb, one for playback),
 * 3 is triple-buffering (one for usb, one for playback, one for queuing), ... */

/* Samples come in (maximum) 1023 byte chunks. Samples are also 16 bits per channel per sample.
 *
 * One buffer holds (1023 / (2Bx2ch)) = 255 (rounded down) samples
 * So the _maximum_ play time per buffer is (255 / sps).
 * For 44100  Hz: 5.7 mS
 * For 48000  Hz: 5.3 mS
 * For 192000 Hz: 1.3 mS
 *
 * From testing on MacOS (likely to be the toughest customer...) on Designware driver
 * we get data every Frame (so, every millisecond).
 *
 * If we get data every millisecond, we need 1mS to transfer 1.3mS of playback
 * in order to sustain 192 kHz playback!
 * At 44.1 kHz, the requirements are much less - 1mS of data transfer for 5.7mS of playback
 * At 48 kHz, 1mS can transfer 5.3mS of playback.
 *
 * It appears that this is "maximum", but we more likely get "enough for 1mS" every millisecond.
 *
 * Working backwards:
 * 44100 Hz: 45 samples transferred every frame (*2ch * 2bytes) = 180 bytes every frame
 * 48000 Hz: 48 samples transferred every frame (*2ch * 2bytes) = 192 bytes every frame
 * 192000 Hz: *2ch *2bytes = 768 bytes every frame
 *
 * We appear to be more limited by our PCM system's need to gobble up data at startup.
 * This may actually, contrary to intuition, make us need a higher number of buffers
 * for _lower_ sample rates, as we will need more buffers' worth of data up-front due to
 * lower amounts of data in each USB frame (assuming the mixer wants the same amount of data upfront
 * regardless of sample rate).
 *
 * Making the executive decision to only export frequencies 44.1k+.
 */
#define NR_BUFFERS      32
#define MINIMUM_BUFFERS_QUEUED 16
/* size of each buffer: must be smaller than 1023 (max isochronous packet size) */
#define BUFFER_SIZE     1023
/* make sure each buffer size is actually a multiple of 32 bytes to avoid any
 * issue with strange alignements */
#define REAL_BUF_SIZE   ALIGN_UP(BUFFER_SIZE, 32)

bool alloc_failed = false;
bool usb_audio_playing = false;
int tmp_saved_vol;

/* buffers used for usb, queuing and playback */
static unsigned char *rx_buffer;
int rx_buffer_handle;
/* buffer size */
static int rx_buf_size[NR_BUFFERS]; /* only used for debug screen counter now */
/* index of the next buffer to play */
static int rx_play_idx;
/* index of the next buffer to fill */
static int rx_usb_idx;
/* playback underflowed ? */
bool playback_audio_underflow;
/* usb overflow ? */
bool usb_rx_overflow;

/* dsp processing buffers */
#define DSP_BUF_SIZE (BUFFER_SIZE*4) /* arbitrarily x4 */
#define REAL_DSP_BUF_SIZE   ALIGN_UP(DSP_BUF_SIZE, 32)
static uint16_t *dsp_buf;
int dsp_buf_handle;
static int dsp_buf_size[NR_BUFFERS];
struct dsp_config *dsp = NULL;

/* feedback variables */
#define USB_FRAME_MAX 0x7FF
#define NR_SAMPLES_HISTORY 32
int32_t samples_fb;
int32_t buffers_filled_old;
long buffers_filled_accumulator;
long buffers_filled_accumulator_old;
int buffers_filled_avgcount;
int buffers_filled_avgcount_old;
static uint8_t sendFf[4] USB_DEVBSS_ATTR;
static bool sent_fb_this_frame = false;
int fb_startframe = 0;
bool send_fb = false;

/* debug screen sample count display variables */
static unsigned long samples_received;
static unsigned long samples_received_last;
int32_t samples_received_report;
int buffers_filled_min;
int buffers_filled_min_last;
int buffers_filled_max;
int buffers_filled_max_last;

/* frame drop recording variables */
static int last_frame = 0;
static int frames_dropped = 0;

/* for blocking normal playback */
static bool usbaudio_active = false;

/* ===== Source mode (iPod -> USB host) TX ring buffer ===== */
/* Max bytes per USB frame: 48000Hz * 2ch * 2bytes / 1000 = 192 bytes */
#define TX_FRAME_SIZE 192
/* Ring buffer size: ~8.7s at 48kHz — large enough to absorb codec
 * decode bursts and I2S vs USB clock drift (44117 vs 44100 Hz).
 * At 70.6 bytes/sec drift, takes ~6.2 hours continuous play to reach
 * the 75% write-throttle threshold from midpoint. */
#define TX_RING_SIZE (TX_FRAME_SIZE * 16384)
/* Pre-buffering threshold: fixed ~24KB (~140ms at 44.1kHz).
 * Must not scale with ring buffer size or startup delay grows. */
#define SOURCE_PREBUF_BYTES (TX_FRAME_SIZE * 128)
static unsigned char *tx_ring_buf;
static int tx_ring_buf_handle;
static volatile int tx_write_pos; /* byte offset, updated by buffer hook */
static volatile int tx_read_pos;  /* byte offset, updated by ISO IN completion */
static bool source_streaming = false;
static bool source_prebuffering = false;
#define SOURCE_FADE_FRAMES 32 /* stereo frames to fade-in over (~0.7ms at 44.1kHz) */
static int source_fade_pos;
static int16_t source_last_sample[2]; /* last L/R sample for fade-out on underflow */
/* Fractional sample accumulator for non-integer sample rates (e.g. 44.1kHz).
 * At 44.1kHz, we need 44.1 samples/frame. We track the fractional remainder
 * and send an extra sample every 10th frame (9x44 + 1x45 = 441 per 10ms). */
static int source_frac_num;  /* accumulated fractional numerator */
static unsigned char tx_send_buf[TX_FRAME_SIZE] USB_DEVBSS_ATTR;
static unsigned char silence_buf[TX_FRAME_SIZE] USB_DEVBSS_ATTR;

/* Schematic view of the RX situation:
 * (in case NR_BUFFERS = 4)
 *
 * +--------+      +--------+      +--------+      +--------+
 * |        |      |        |      |        |      |        |
 * | buf[0] | ---> | buf[1] | ---> | buf[2] | ---> | buf[3] | ---> (back to buf[0])
 * |        |      |        |      |        |      |        |
 * +--------+      +--------+      +--------+      +--------+
 *     ^               ^               ^               ^
 *     |               |               |               |
 * rx_play_idx      (buffer         rx_usb_idx      (empty buffer)
 * (buffer being     filled)        (buffer being
 *  played)                          filled)
 *
 * Error handling:
 * in the RX situation, there are two possible errors
 * - playback underflow: playback wants more data but we don't have any to
 *   provide, so we have to stop audio and wait for some prebuffering before
 *   starting again
 * - usb overflow: usb wants to send more data but don't have any more free buffers,
 *   so we have to pause usb reception and wait for some playback buffer to become
 *   free again
 */

/* USB Audio encodes frequencies with 3 bytes... */
static void encode3(uint8_t arr[3], unsigned long freq)
{
    /* ugly */
    arr[0] = freq & 0xff;
    arr[1] = (freq >> 8) & 0xff;
    arr[2] = (freq >> 16) & 0xff;
}

static unsigned long decode3(uint8_t arr[3])
{
    return arr[0] | (arr[1] << 8) | (arr[2] << 16);
}

/* size is samples per frame! */
static void encodeFBfixedpt(uint8_t arr[4], int32_t value, bool portspeed)
{
    uint32_t fixedpt;
    /* high-speed */
    if (portspeed)
    {
        /* Q16.16 */
        fixedpt = value;

        arr[0] = (fixedpt & 0xFF);
        arr[1] = (fixedpt>>8) & 0xFF;
        arr[2] = (fixedpt>>16) & 0xFF;
        arr[3] = (fixedpt>>24) & 0xFF;
    }
    else /* full-speed */
    {
        /* Q16.16 --> Q10.10 --> Q10.14 */
        fixedpt = value / (1<<2); /* convert from Q16.16 to Q10.14 */

        /* then aligned so it's more like Q10.14 */
        /* NOTE: this line left for posterity */
        /* fixedpt = fixedpt << (4); */

        arr[0] = (fixedpt & 0xFF);
        arr[1] = (fixedpt>>8) & 0xFF;
        arr[2] = (fixedpt>>16) & 0xFF;
    }

}

static void set_playback_sampling_frequency(unsigned long f)
{
    /* only values 44.1k and higher (array is in descending order) */
    for(int i = 0; i <= HW_FREQ_44; i++)
    {
        /* compare errors */
        int err = abs((long)hw_freq_sampr[i] - (long)f);
        int best_err = abs((long)hw_freq_sampr[as_playback_freq_idx] - (long)f);
        if(err < best_err)
            as_playback_freq_idx = i;
    }

    logf("usbaudio: set playback sampling frequency to %lu Hz for a requested %lu Hz",
        hw_freq_sampr[as_playback_freq_idx], f);

    mixer_set_frequency(hw_freq_sampr[as_playback_freq_idx]);
    pcm_apply_settings();
}

unsigned long usb_audio_get_playback_sampling_frequency(void)
{
    /* logf("usbaudio: get playback sampl freq %lu Hz", hw_freq_sampr[as_playback_freq_idx]); */
    return hw_freq_sampr[as_playback_freq_idx];
}

void usb_audio_init(void)
{
    unsigned int i;
    /* initialized tSamFreq array */
    logf("usbaudio: (init) supported frequencies");
    /* playback: only values 44.1k and higher (array is in descending order) */
    for(i = 0; i <= HW_FREQ_44; i++)
    {
        logf("usbaudio: playback %lu Hz", hw_freq_sampr[i]);
        encode3(as_playback_format_type_i.tSamFreq[i], hw_freq_sampr[i]);
    }
    /* source: all hardware-supported rates in ascending order (matches Apple layout) */
    for(i = 0; i < HW_NUM_FREQ; i++)
    {
        /* hw_freq_sampr is descending; reverse into ascending for Apple compatibility */
        int src_idx = HW_NUM_FREQ - 1 - i;
        logf("usbaudio: source %lu Hz", hw_freq_sampr[src_idx]);
        encode3(as_source_format_type_i.tSamFreq[i], hw_freq_sampr[src_idx]);
    }
}

int usb_audio_request_buf(void)
{
    /* In config 2 (DAC mode), playback must continue — source mode streams it.
     * In config 1 (PC mode), audio is not active so this is not called.
     * audio_stop() is not needed here; the SYS_USB_CONNECTED broadcast
     * handles playback shutdown when mass storage takes exclusive access. */

    /* Only allocate sink buffers if sink endpoints were actually assigned */
    if (EP_ISO_OUT != 0)
    {
        /* attempt to allocate the receive buffers */
        rx_buffer_handle = core_alloc(REAL_BUF_SIZE);
        if (rx_buffer_handle < 0)
        {
            alloc_failed = true;
            return -1;
        }
        else
        {
            alloc_failed = false;
            core_pin(rx_buffer_handle);
            rx_buffer = core_get_data(rx_buffer_handle);
        }

        dsp_buf_handle = core_alloc(NR_BUFFERS * REAL_DSP_BUF_SIZE);
        if (dsp_buf_handle < 0)
        {
            alloc_failed = true;
            rx_buffer_handle = core_free(rx_buffer_handle);
            rx_buffer = NULL;
            return -1;
        }
        else
        {
            alloc_failed = false;
            core_pin(dsp_buf_handle);
            dsp_buf = core_get_data(dsp_buf_handle);
        }
    }

    /* allocate TX ring buffer for source mode */
    tx_ring_buf_handle = core_alloc(TX_RING_SIZE);
    if (tx_ring_buf_handle < 0)
    {
        alloc_failed = true;
        if (rx_buffer)
        {
            rx_buffer_handle = core_free(rx_buffer_handle);
            rx_buffer = NULL;
        }
        if (dsp_buf)
        {
            dsp_buf_handle = core_free(dsp_buf_handle);
            dsp_buf = NULL;
        }
        return -1;
    }
    else
    {
        core_pin(tx_ring_buf_handle);
        tx_ring_buf = core_get_data(tx_ring_buf_handle);
    }

    /* logf("usbaudio: got buffer"); */
    return 0;
}

void usb_audio_free_buf(void)
{
    /* logf("usbaudio: free buffer"); */
    if (rx_buffer)
    {
        rx_buffer_handle = core_free(rx_buffer_handle);
        rx_buffer = NULL;
    }

    if (dsp_buf)
    {
        dsp_buf_handle = core_free(dsp_buf_handle);
        dsp_buf = NULL;
    }

    if (tx_ring_buf)
    {
        tx_ring_buf_handle = core_free(tx_ring_buf_handle);
        tx_ring_buf = NULL;
    }
}

unsigned int usb_audio_get_out_ep(void)
{
    return EP_ISO_OUT;
}

unsigned int usb_audio_get_in_ep(void)
{
    return EP_ISO_FEEDBACK_IN;
}

int usb_audio_set_first_interface(int interface)
{
    usb_interface = interface;
    logf("usbaudio: usb_interface=%d", usb_interface);
    return interface + 2; /* Audio Control + Source Streaming */
}

int usb_audio_get_config_descriptor(unsigned char *dest, int max_packet_size)
{
    (void)max_packet_size;
    unsigned int i;
    unsigned char *orig_dest = dest;

    logf("get config descriptors (source-only)");

    /* Config 2 is source-only, matching Apple/ipod-gadget layout:
     * AC interface (bInCollection=1) + Source AS + (iAP HID is separate driver) */

    /* Audio Control interface */
    ac_interface.bInterfaceNumber = usb_interface;

    /* Source-only AC header */
    ac_source_header.baInterfaceNr[0] = usb_interface + 1;
    ac_source_header.wTotalLength = 0;
    for(i = 0; i < AC_SOURCE_CS_DESCRIPTORS_LIST_SIZE; i++)
        ac_source_header.wTotalLength += ac_source_cs_descriptors_list[i]->bLength;

    /* Source AS interface numbers */
    as_interface_alt_idle_source.bInterfaceNumber = usb_interface + 1;
    as_interface_alt_source.bInterfaceNumber = usb_interface + 1;

    /* Source endpoint */
    as_iso_audio_in_ep.bEndpointAddress = EP_ISO_SOURCE_IN;
    as_iso_audio_in_ep.bInterval = usb_drv_port_speed() ? 4 : 1;

    logf("usbaudio: port_speed=%s", usb_drv_port_speed()?"hs":"fs");

    /* Pack source-only descriptors */
    for(i = 0; i < USB_SOURCE_DESCRIPTORS_LIST_SIZE; i++)
    {
        memcpy(dest, usb_source_descriptors_list[i],
               usb_source_descriptors_list[i]->bLength);
        dest += usb_source_descriptors_list[i]->bLength;
    }

    return dest - orig_dest;
}

static void playback_audio_get_more(const void **start, size_t *size)
{
    /* if there are no more filled buffers, playback has just underflowed */
    if(rx_play_idx == rx_usb_idx)
    {
        logf("usbaudio: playback underflow");
        playback_audio_underflow = true;
        *start = NULL;
        *size = 0;
        return;
    }

    /* give buffer and advance */
    logf("usbaudio: buf adv");
    *start = dsp_buf + (rx_play_idx * REAL_DSP_BUF_SIZE/sizeof(*dsp_buf));
    *size = dsp_buf_size[rx_play_idx];
    rx_play_idx = (rx_play_idx + 1) % NR_BUFFERS;

    /* if usb RX buffers had overflowed, we can start to receive again
     * guard against IRQ to avoid race with completion usb completion (although
     * this function is probably running in IRQ context anyway) */
    int oldlevel = disable_irq_save();
    if(usb_rx_overflow)
    {
        logf("usbaudio: recover usb rx overflow");
        usb_rx_overflow = false;
        usb_drv_recv_nonblocking(EP_ISO_OUT, rx_buffer, BUFFER_SIZE);
    }
    restore_irq(oldlevel);
}

static void __attribute__((unused)) usb_audio_start_playback(void)
{
    usb_audio_playing = true;
    usb_rx_overflow = false;
    playback_audio_underflow = true;
    rx_play_idx = 0;
    rx_usb_idx = 0;

    /* feedback initialization */
    fb_startframe = usb_drv_get_frame_number();
    samples_fb = 0;
    samples_received_report = 0;

    /* debug screen info - frame drop counter */
    frames_dropped = 0;
    last_frame = -1;
    buffers_filled_min = -1;
    buffers_filled_min_last = -1;
    buffers_filled_max = -1;
    buffers_filled_max_last = -1;

    /* debug screen info - sample counters */
    samples_received = 0;
    samples_received_last = 0;

    /* TODO: implement recording from the USB stream */
#if (INPUT_SRC_CAPS != 0)
    audio_set_input_source(AUDIO_SRC_PLAYBACK, SRCF_PLAYBACK);
    audio_set_output_source(AUDIO_SRC_PLAYBACK);
#endif
    logf("usbaudio: start playback at %lu Hz", hw_freq_sampr[as_playback_freq_idx]);

    /* Configure DSP for sink mode (USB audio received from host) */
    dsp = dsp_get_config(CODEC_IDX_AUDIO);
    dsp_configure(dsp, DSP_RESET, 0);
    dsp_configure(dsp, DSP_SET_STEREO_MODE, STEREO_INTERLEAVED);
    dsp_configure(dsp, DSP_SET_SAMPLE_DEPTH, 16);
#ifdef HAVE_PITCHCONTROL
    sound_set_pitch(PITCH_SPEED_100);
    dsp_set_timestretch(PITCH_SPEED_100);
#endif
    set_playback_sampling_frequency(HW_SAMPR_DEFAULT);

    mixer_set_frequency(hw_freq_sampr[as_playback_freq_idx]);
    pcm_apply_settings();
    mixer_channel_set_amplitude(PCM_MIXER_CHAN_USBAUDIO, MIX_AMP_UNITY);

    usb_drv_recv_nonblocking(EP_ISO_OUT, rx_buffer, BUFFER_SIZE);
}

static void usb_audio_stop_playback(void)
{
    logf("usbaudio: stop playback");
    if(usb_audio_playing)
    {
        mixer_channel_stop(PCM_MIXER_CHAN_USBAUDIO);
        usb_audio_playing = false;
    }
    send_fb = false;
}

/* ===== Source mode (iPod -> USB host) ===== */

/*
 * Compute number of bytes to send in this USB frame.
 * Handles non-integer sample rates (e.g. 44100 Hz / 1000 = 44.1 samples/frame)
 * using a fractional accumulator. Uses the nominal frequency (what the DAC
 * expects via USB descriptor). The write-side throttle in source_buffer_hook()
 * handles the I2S vs USB clock drift.
 */
static int source_frame_bytes(void)
{
    unsigned long freq = hw_freq_sampr[as_source_freq_idx];
    int base_samples = freq / 1000;
    int remainder = freq % 1000;
    int samples;

    source_frac_num += remainder;
    if (source_frac_num >= 1000)
    {
        source_frac_num -= 1000;
        samples = base_samples + 1;
    }
    else
    {
        samples = base_samples;
    }

    return samples * 4; /* stereo 16-bit: 2 channels * 2 bytes */
}

static void set_source_sampling_frequency(unsigned long f)
{
    for(int i = 0; i < HW_NUM_FREQ; i++)
    {
        int err = abs((long)hw_freq_sampr[i] - (long)f);
        int best_err = abs((long)hw_freq_sampr[as_source_freq_idx] - (long)f);
        if(err < best_err)
            as_source_freq_idx = i;
    }

    logf("usbaudio: set source sampling frequency to %lu Hz for a requested %lu Hz",
        hw_freq_sampr[as_source_freq_idx], f);
}

static void source_buffer_hook(const void *start, size_t size)
{
    if (!source_streaming || !tx_ring_buf)
        return;

    /* compute available space in ring buffer */
    int write = tx_write_pos;
    int read = tx_read_pos;
    int space;
    if (write >= read)
        space = TX_RING_SIZE - (write - read) - 1;
    else
        space = read - write - 1;

    int to_copy = MIN((int)size, space);
    to_copy &= ~3; /* round down to sample frame boundary (4 bytes) */

    /* Write-side drift safety: when ring buffer is >75% full,
     * skip 1 stereo sample from input to prevent overflow.
     * Keeps USB packet sizes constant for MFi DAC compatibility.
     * With actual I2S rate, this only fires after hours of
     * continuous play under worst-case crystal mismatch. */
    if (to_copy > 4)
    {
        int used = TX_RING_SIZE - 1 - space;
        if (used > TX_RING_SIZE * 3 / 4)
            to_copy -= 4;
    }

    if (to_copy <= 0)
        return;

    const unsigned char *src = (const unsigned char *)start;

    /* copy with wraparound */
    int first = MIN(to_copy, TX_RING_SIZE - write);
    memcpy(tx_ring_buf + write, src, first);
    if (to_copy > first)
        memcpy(tx_ring_buf, src + first, to_copy - first);

    tx_write_pos = (write + to_copy) % TX_RING_SIZE;
}

static void usb_audio_start_source(void)
{
    logf("usbaudio: start source at %lu Hz ep=0x%02X", hw_freq_sampr[as_source_freq_idx], EP_ISO_SOURCE_IN);
    source_streaming = true;
    source_prebuffering = true;
    source_fade_pos = SOURCE_FADE_FRAMES; /* no fade during initial silence */
    source_last_sample[0] = 0;
    source_last_sample[1] = 0;
    tx_write_pos = 0;
    tx_read_pos = 0;
    source_frac_num = 0;
    memset(silence_buf, 0, sizeof(silence_buf));
    memset(tx_send_buf, 0, sizeof(tx_send_buf));

    /* register hook to capture playback audio */
    mixer_channel_set_buffer_hook(PCM_MIXER_CHAN_PLAYBACK, source_buffer_hook);

    /* start the ISO IN chain with a silence frame */
    int frame_bytes = source_frame_bytes();
    usb_drv_send_nonblocking(EP_ISO_SOURCE_IN, silence_buf, frame_bytes);

#ifdef HAVE_CS42L55
    /* Power down headphone amps — audio goes over USB, not the jack */
    audiohw_set_hp_power(false);
#endif
}

static void usb_audio_stop_source(void)
{
    logf("usbaudio: stop source");
    source_streaming = false;
    mixer_channel_set_buffer_hook(PCM_MIXER_CHAN_PLAYBACK, NULL);

#ifdef HAVE_CS42L55
    /* Restore headphone amp power — HP volume regs are intact */
    audiohw_set_hp_power(true);
#endif
}

int usb_audio_set_interface(int intf, int alt)
{
    if(intf == usb_interface)
    {
        if(alt != 0)
        {
            logf("usbaudio: control interface has no alternate %d", alt);
            return -1;
        }

        return 0;
    }
    if(intf == (usb_interface + 1))
    {
        /* Source AS interface (config 2 is source-only) */
        if(alt < 0 || alt > 1)
        {
            logf("usbaudio: source interface has no alternate %d", alt);
            return -1;
        }
        usb_as_source_intf_alt = alt;

        if(usb_as_source_intf_alt == 1)
            usb_audio_start_source();
        else
            usb_audio_stop_source();
        logf("usbaudio: use source alternate %d", alt);

        return 0;
    }
    else
    {
        logf("usbaudio: interface %d has no alternate", intf);
        return -1;
    }
}

int usb_audio_get_interface(int intf)
{
    if(intf == usb_interface)
    {
        logf("usbaudio: control interface alternate is 0");
        return 0;
    }
    else if(intf == (usb_interface + 1))
    {
        logf("usbaudio: source interface alternate is %d", usb_as_source_intf_alt);
        return usb_as_source_intf_alt;
    }
    else
    {
        logf("usbaudio: unknown interface %d", intf);
        return -1;
    }
}

int usb_audio_get_main_intf(void)
{
    return usb_interface;
}

int usb_audio_get_alt_intf(void)
{
    return usb_as_playback_intf_alt;
}

int32_t usb_audio_get_samplesperframe(void)
{
    return samples_fb;
}

int32_t usb_audio_get_samples_rx_perframe(void)
{
    return samples_received_report;
}

static bool usb_audio_as_ctrldata_endpoint_request(struct usb_ctrlrequest* req, void *reqdata)
{
    /* only support sampling frequency */
    if(req->wValue != (USB_AS_EP_CS_SAMPLING_FREQ_CTL << 8))
    {
        logf("usbaudio: endpoint only handles sampling frequency control");
        return false;
    }

    switch(req->bRequest)
    {
        case USB_AC_SET_CUR:
            if(req->wLength != 3)
            {
                logf("usbaudio: bad length for SET_CUR");
                usb_drv_control_response(USB_CONTROL_STALL, NULL, 0);
                return true;
            }
            logf("usbaudio: SET_CUR sampling freq");

            if (reqdata) { /* control write, second pass */
                set_playback_sampling_frequency(decode3(reqdata));
                usb_drv_control_response(USB_CONTROL_ACK, NULL, 0);
                return true;
            } else { /* control write, first pass */
                bool error = false;

                if (req->wLength != 3)
                    error = true;
                    /* ... other validation? */

                if (error)
                    usb_drv_control_response(USB_CONTROL_STALL, NULL, 0);
                else
                    usb_drv_control_response(USB_CONTROL_RECEIVE, usb_buffer, 3);

                return true;
            }

        case USB_AC_GET_CUR:
            if(req->wLength != 3)
            {
                logf("usbaudio: bad length for GET_CUR");
                usb_drv_control_response(USB_CONTROL_STALL, NULL, 0);
                return true;
            }
            logf("usbaudio: GET_CUR sampling freq");
            encode3(usb_buffer, usb_audio_get_playback_sampling_frequency());
            usb_drv_control_response(USB_CONTROL_ACK, usb_buffer, req->wLength);

            return true;

        default:
            logf("usbaudio: unhandled ep req 0x%x", req->bRequest);
    }

    return true;
}

static bool usb_audio_source_endpoint_request(struct usb_ctrlrequest* req, void *reqdata)
{
    /* only support sampling frequency */
    if(req->wValue != (USB_AS_EP_CS_SAMPLING_FREQ_CTL << 8))
    {
        logf("usbaudio: source ep only handles sampling frequency control");
        return false;
    }

    switch(req->bRequest)
    {
        case USB_AC_SET_CUR:
            if(req->wLength != 3)
            {
                usb_drv_control_response(USB_CONTROL_STALL, NULL, 0);
                return true;
            }
            if (reqdata) {
                set_source_sampling_frequency(decode3(reqdata));
                usb_drv_control_response(USB_CONTROL_ACK, NULL, 0);
                return true;
            } else {
                usb_drv_control_response(USB_CONTROL_RECEIVE, usb_buffer, 3);
                return true;
            }

        case USB_AC_GET_CUR:
            if(req->wLength != 3)
            {
                usb_drv_control_response(USB_CONTROL_STALL, NULL, 0);
                return true;
            }
            encode3(usb_buffer, hw_freq_sampr[as_source_freq_idx]);
            usb_drv_control_response(USB_CONTROL_ACK, usb_buffer, req->wLength);
            return true;

        default:
            logf("usbaudio: unhandled source ep req 0x%x", req->bRequest);
    }

    return true;
}

static bool usb_audio_endpoint_request(struct usb_ctrlrequest* req, void *reqdata)
{
    int ep = req->wIndex & 0xff;

    if(ep == EP_ISO_OUT)
        return usb_audio_as_ctrldata_endpoint_request(req, reqdata);
    else if(ep == EP_ISO_SOURCE_IN)
        return usb_audio_source_endpoint_request(req, reqdata);
    else
    {
        logf("usbaudio: unhandled ep req (ep=%d)", ep);
        return false;
    }
}

static bool feature_unit_set_mute(int value, uint8_t cmd)
{
    if(cmd != USB_AC_CUR_REQ)
    {
        logf("usbaudio: feature unit MUTE control only has a CUR setting");
        return false;
    }

    if(value == 1)
    {
        logf("usbaudio: mute !");
        tmp_saved_vol = sound_current(SOUND_VOLUME);

        /* setvol does range checking for us! */
        global_status.volume = sound_min(SOUND_VOLUME);
        setvol();
        return true;
    }
    else if(value == 0)
    {
        logf("usbaudio: not muted !");

        /* setvol does range checking for us! */
        global_status.volume = tmp_saved_vol;
        setvol();
        return true;
    }
    else
    {
        logf("usbaudio: invalid value for CUR setting of feature unit (%d)", value);
        return false;
    }
}

static bool feature_unit_get_mute(int *value, uint8_t cmd)
{
    if(cmd != USB_AC_CUR_REQ)
    {
        logf("usbaudio: feature unit MUTE control only has a CUR setting");
        return false;
    }

    *value = (sound_current(SOUND_VOLUME) == sound_min(SOUND_VOLUME));
    return true;
}

/*
* USB volume is a signed 16-bit value, -127.9961 dB (0x8001) to +127.9961 dB (0x7FFF)
* in steps of 1/256 dB (0.00390625 dB)
*
* We need to account for different devices having different numbers of decimals
*/
/* TODO: do we need to explicitly round these? Will we have a "walking" round conversion issue? */
/* Step values of 1 dB (and multiples), and 0.5 dB should be able to be met exactly, */
/* presuming that it starts on an even number. */
static int usb_audio_volume_to_db(int vol, int numdecimals)
{
    int tmp = (signed long)((signed short)vol * ipow(10, numdecimals)) / 256;
    /* logf("vol=0x%04X, numdecimals=%d, tmp=%d", vol, numdecimals, tmp); */
    return tmp;
}
static int db_to_usb_audio_volume(int db, int numdecimals)
{
    int tmp = (signed long)(db * 256) / ipow(10, numdecimals);
    /* logf("db=%d, numdecimals=%d, tmpTodB=%d", db, numdecimals, usb_audio_volume_to_db(tmp, numdecimals)); */
    return tmp;
}

#if defined(DEBUG) || (defined(LOGF_ENABLE) && defined(ROCKBOX_HAS_LOGF))
static const char *usb_audio_ac_ctl_req_str(uint8_t cmd)
{
    switch(cmd)
    {
        case USB_AC_CUR_REQ: return "CUR";
        case USB_AC_MIN_REQ: return "MIN";
        case USB_AC_MAX_REQ: return "MAX";
        case USB_AC_RES_REQ: return "RES";
        case USB_AC_MEM_REQ: return "MEM";
        default: return "<unknown>";
    }
}
#endif

static bool feature_unit_set_volume(int value, uint8_t cmd)
{
    if(cmd != USB_AC_CUR_REQ)
    {
        logf("usbaudio: feature unit VOLUME doesn't support %s setting", usb_audio_ac_ctl_req_str(cmd));
        return false;
    }

    logf("usbaudio: set volume=%d dB", usb_audio_volume_to_db(value, sound_numdecimals(SOUND_VOLUME)));

    /* setvol does range checking for us! */
    /* we cannot guarantee the host will send us a volume within our range */
    global_status.volume = usb_audio_volume_to_db(value, sound_numdecimals(SOUND_VOLUME));
    setvol();
    return true;
}

static bool feature_unit_get_volume(int *value, uint8_t cmd)
{
    switch(cmd)
    {
        case USB_AC_CUR_REQ: *value = db_to_usb_audio_volume(sound_current(SOUND_VOLUME), sound_numdecimals(SOUND_VOLUME)); break;
        case USB_AC_MIN_REQ: *value = db_to_usb_audio_volume(sound_min(SOUND_VOLUME), sound_numdecimals(SOUND_VOLUME)); break;
        case USB_AC_MAX_REQ: *value = db_to_usb_audio_volume(sound_max(SOUND_VOLUME), sound_numdecimals(SOUND_VOLUME)); break;
        case USB_AC_RES_REQ: *value = db_to_usb_audio_volume(sound_steps(SOUND_VOLUME), sound_numdecimals(SOUND_VOLUME)); break;
        default:
            logf("usbaudio: feature unit VOLUME doesn't support %s setting", usb_audio_ac_ctl_req_str(cmd));
            return false;
    }

    /* logf("usbaudio: get %s volume=%d dB", usb_audio_ac_ctl_req_str(cmd), usb_audio_volume_to_db(*value, sound_numdecimals(SOUND_VOLUME))); */
    return true;
}

int usb_audio_get_cur_volume(void)
{
    int vol;
    feature_unit_get_volume(&vol, USB_AC_CUR_REQ);
    return usb_audio_volume_to_db(vol, sound_numdecimals(SOUND_VOLUME));
}

static bool usb_audio_set_get_feature_unit(struct usb_ctrlrequest* req, void *reqdata)
{
    int channel = req->wValue & 0xff;
    int selector = req->wValue >> 8;
    uint8_t cmd = (req->bRequest & ~USB_AC_GET_REQ);
    int value = 0;
    int i;
    bool handled;

    /* master channel only */
    if(channel != 0)
    {
        logf("usbaudio: set/get on feature unit only apply to master channel (%d)", channel);
        return false;
    }
    /* selectors */
    /* all send/received values are integers already - read data if necessary and store in it in an integer */
    if(req->bRequest & USB_AC_GET_REQ)
    {
        /* get */
        switch(selector)
        {
            case USB_AC_FU_MUTE:
                handled = (req->wLength == 1) && feature_unit_get_mute(&value, cmd);
                break;
            case USB_AC_VOLUME_CONTROL:
                handled = (req->wLength == 2) && feature_unit_get_volume(&value, cmd);
                break;
            default:
                handled = false;
                logf("usbaudio: unhandled control selector of feature unit (0x%x)", selector);
                break;
        }

        if(!handled)
        {
            logf("usbaudio: unhandled get control 0x%x selector 0x%x of feature unit", cmd, selector);
            usb_drv_control_response(USB_CONTROL_STALL, NULL, 0);
            return true;
        }

        if(req->wLength == 0 || req->wLength > 4)
        {
            logf("usbaudio: get data payload size is invalid (%d)", req->wLength);
            return false;
        }

        for(i = 0; i < req->wLength; i++)
            usb_buffer[i] = (value >> (8 * i)) & 0xff;

        usb_drv_control_response(USB_CONTROL_ACK, usb_buffer, req->wLength);
        return true;
    }
    else
    {
        /* set */
        if(req->wLength == 0 || req->wLength > 4)
        {
            logf("usbaudio: set data payload size is invalid (%d)", req->wLength);
            return false;
        }

        if (reqdata) {

            for(i = 0; i < req->wLength; i++)
                value = value | (usb_buffer[i] << (i * 8));

            switch(selector)
            {
                case USB_AC_FU_MUTE:
                    handled = (req->wLength == 1) && feature_unit_set_mute(value, cmd);
                    break;
                case USB_AC_VOLUME_CONTROL:
                    handled = (req->wLength == 2) && feature_unit_set_volume(value, cmd);
                    break;
                default:
                    handled = false;
                    logf("usbaudio: unhandled control selector of feature unit (0x%x)", selector);
                    break;
            }

            if(!handled)
            {
                logf("usbaudio: unhandled set control 0x%x selector 0x%x of feature unit", cmd, selector);
                usb_drv_control_response(USB_CONTROL_STALL, NULL, 0);
                return true;
            }

            usb_drv_control_response(USB_CONTROL_ACK, NULL, 0);
            return true;
        } else {
            /*
             * should handle the following (req->wValue >> 8):
             * USB_AC_FU_MUTE
             * USB_AC_VOLUME_CONTROL
             */

            bool error = false;

            if (error)
                usb_drv_control_response(USB_CONTROL_STALL, NULL, 0);
            else
                usb_drv_control_response(USB_CONTROL_RECEIVE, usb_buffer, 3);

            return true;
        }

        return true;
    }
}

static bool usb_audio_ac_set_get_request(struct usb_ctrlrequest* req, void *reqdata)
{
    switch(req->wIndex >> 8)
    {
        case AC_PLAYBACK_FEATURE_ID:
            return usb_audio_set_get_feature_unit(req, reqdata);
        default:
            logf("usbaudio: unhandled set/get on entity %d", req->wIndex >> 8);
            return false;
    }
}

static bool usb_audio_interface_request(struct usb_ctrlrequest* req, void *reqdata)
{
    int intf = req->wIndex & 0xff;

    if(intf == usb_interface)
    {
        switch(req->bRequest)
        {
            case USB_AC_SET_CUR: case USB_AC_SET_MIN: case USB_AC_SET_MAX: case USB_AC_SET_RES:
            case USB_AC_SET_MEM: case USB_AC_GET_CUR: case USB_AC_GET_MIN: case USB_AC_GET_MAX:
            case USB_AC_GET_RES: case USB_AC_GET_MEM:
                return usb_audio_ac_set_get_request(req, reqdata);
            default:
                logf("usbaudio: unhandled ac intf req 0x%x", req->bRequest);
                return false;
        }
    }
    else
    {
        logf("usbaudio: unhandled intf req (intf=%d)", intf);
        return false;
    }
}

bool usb_audio_control_request(struct usb_ctrlrequest* req, void *reqdata, unsigned char* dest)
{
    (void) reqdata;
    (void) dest;

    switch(req->bRequestType & USB_RECIP_MASK)
    {
        case USB_RECIP_ENDPOINT:
            return usb_audio_endpoint_request(req, reqdata);
        case USB_RECIP_INTERFACE:
            return usb_audio_interface_request(req, reqdata);
        default:
            logf("usbaudio: unhandled req type 0x%x", req->bRequestType);
            return false;
    }
}

void usb_audio_init_connection(void)
{
    logf("usbaudio: init connection");

    /* make sure we can get the buffers first... */
    /* TODO: disable this driver when failed */
    if (usb_audio_request_buf())
        return;

    usbaudio_active = true;

    /* DSP setup deferred to usb_audio_start_playback() (sink mode only).
     * In source mode, the codec's DSP must not be disturbed — it is
     * actively processing audio that source mode captures via the
     * mixer buffer hook. */

    usb_as_playback_intf_alt = 0;
    tmp_saved_vol = sound_current(SOUND_VOLUME);
    usb_audio_playing = false;

    /* source mode init */
    usb_as_source_intf_alt = 0;
    as_source_freq_idx = HW_FREQ_44; /* default to 44.1kHz */
    source_streaming = false;
    source_frac_num = 0;
    tx_write_pos = 0;
    tx_read_pos = 0;
}

void usb_audio_disconnect(void)
{
    logf("usbaudio: disconnect");

    if(!usbaudio_active)
        return;

    usb_audio_stop_playback();
    usb_audio_stop_source();
    usb_audio_free_buf();
    usbaudio_active = false;
}

bool usb_audio_get_active(void)
{
    return usbaudio_active;
}

bool usb_audio_get_alloc_failed(void)
{
    return alloc_failed;
}

bool usb_audio_get_playing(void)
{
    return usb_audio_playing;
}

/* determine if enough prebuffering has been done to restart audio */
bool prebuffering_done(void)
{
    /* restart audio if at least MINIMUM_BUFFERS_QUEUED buffers are filled */
    int diff = (rx_usb_idx - rx_play_idx + NR_BUFFERS) % NR_BUFFERS;
    return diff >= MINIMUM_BUFFERS_QUEUED;
}

int usb_audio_get_prebuffering(void)
{
    return (rx_usb_idx - rx_play_idx + NR_BUFFERS) % NR_BUFFERS;
}

int32_t usb_audio_get_prebuffering_avg(void)
{
    if (buffers_filled_avgcount == 0)
    {
        return TO_16DOT16_FIXEDPT(usb_audio_get_prebuffering());
    } else {
        return (TO_16DOT16_FIXEDPT(buffers_filled_accumulator)/buffers_filled_avgcount) + TO_16DOT16_FIXEDPT(MINIMUM_BUFFERS_QUEUED);
    }
}

int usb_audio_get_prebuffering_maxmin(bool max)
{
    if (max)
    {
        return buffers_filled_max == -1 ? buffers_filled_max_last : buffers_filled_max;
    }
    else
    {
        return buffers_filled_min == -1 ? buffers_filled_min_last : buffers_filled_min;
    }
}

bool usb_audio_get_underflow(void)
{
    return playback_audio_underflow;
}

bool usb_audio_get_overflow(void)
{
    return usb_rx_overflow;
}

int usb_audio_get_frames_dropped(void)
{
    return frames_dropped;
}

void usb_audio_transfer_complete(int ep, int dir, int status, int length)
{
    /* normal handler is too slow to handle the completion rate, because
     * of the low thread schedule rate */
    (void) ep;
    (void) dir;
    (void) status;
    (void) length;
}

bool usb_audio_fast_transfer_complete(int ep, int dir, int status, int length)
{
    (void) dir;
    bool retval = false;

    if(ep == EP_NUM(EP_ISO_OUT) && usb_as_playback_intf_alt == 1)
    {
        /* check for dropped frames */
        if (last_frame != usb_drv_get_frame_number())
        {
            if ((((last_frame + 1) % (USB_FRAME_MAX + 1)) != usb_drv_get_frame_number()) && (last_frame != -1))
            {
                frames_dropped++;
            }
            last_frame = usb_drv_get_frame_number();
        }

        /* If audio and feedback EPs happen to have the same base number (with opposite directions, of course), */
        /* we will get replies to the feedback here, don't want that to be interpreted as data. */
        if (length <= 4)
        {
            return true;
        }

        logf("usbaudio: frame: %d bytes: %d", usb_drv_get_frame_number(), length);
        if(status != 0)
            return true; /* FIXME how to handle error here ? */

        /* store length, queue buffer */
        rx_buf_size[rx_usb_idx] = length;

        /* debug screen counter */
        samples_received = samples_received + length;

        /* process through DSP right away! */
        struct dsp_buffer src;
        src.remcount = length/4; /* in samples */
        src.pin[0] = rx_buffer;
        src.proc_mask = 0;

        struct dsp_buffer dst;
        dst.remcount = 0;
        dst.bufcount = DSP_BUF_SIZE/4; /* in samples */
        dst.p16out = dsp_buf + (rx_usb_idx * REAL_DSP_BUF_SIZE/sizeof(*dsp_buf)); /* array index */

        dsp_process(dsp, &src, &dst, false);
        dsp_buf_size[rx_usb_idx] = dst.remcount * 2 * sizeof(*dsp_buf); /* need value in bytes */

        rx_usb_idx = (rx_usb_idx + 1) % NR_BUFFERS;

        /* guard against IRQ to avoid race with completion audio completion */
        int oldlevel = disable_irq_save();
        /* setup a new transaction except if we ran out of buffers */
        if(rx_usb_idx != rx_play_idx)
        {
            logf("usbaudio: new transaction");
            usb_drv_recv_nonblocking(EP_ISO_OUT, rx_buffer, BUFFER_SIZE);
        }
        else
        {
            logf("usbaudio: rx overflow");
            usb_rx_overflow = true;
        }
        /* if audio underflowed and prebuffering is done, restart audio */
        if(playback_audio_underflow && prebuffering_done())
        {
            logf("usbaudio: prebuffering done");
            playback_audio_underflow = false;
            usb_rx_overflow = false;
            mixer_channel_play_data(PCM_MIXER_CHAN_USBAUDIO, playback_audio_get_more, NULL, 0);
        }
        restore_irq(oldlevel);
        retval =  true;
    }
    else
    {
        retval = false;
    }

    /* Source mode: handle ISO IN completion for audio data */
    if(ep == EP_NUM(EP_ISO_SOURCE_IN) && source_streaming)
    {
        int frame_bytes = source_frame_bytes();

        /* compute available data in ring buffer */
        int write = tx_write_pos;
        int read = tx_read_pos;
        int available;
        if (write >= read)
            available = write - read;
        else
            available = TX_RING_SIZE - read + write;

        /* pre-buffering: wait until ring buffer has enough cushion
         * before sending real data, to avoid underflow clicks */
        if (source_prebuffering && available >= SOURCE_PREBUF_BYTES)
        {
            source_prebuffering = false;
            source_fade_pos = 0; /* start fade-in from silence to audio */
        }

        if (source_prebuffering)
        {
            usb_drv_send_nonblocking(EP_ISO_SOURCE_IN,
                                     silence_buf, frame_bytes);
        }
        else if (available >= frame_bytes)
        {
            /* copy from ring buffer to send buffer with wraparound */
            int first = MIN(frame_bytes, TX_RING_SIZE - read);
            memcpy(tx_send_buf, tx_ring_buf + read, first);
            if (frame_bytes > first)
                memcpy(tx_send_buf + first, tx_ring_buf, frame_bytes - first);
            tx_read_pos = (read + frame_bytes) % TX_RING_SIZE;

            /* fade-in: ramp from silence to full amplitude to avoid
             * a waveform discontinuity click at playback start */
            if (source_fade_pos < SOURCE_FADE_FRAMES)
            {
                int16_t *s = (int16_t *)tx_send_buf;
                int n = frame_bytes / 4; /* stereo sample frames */
                int i;
                for (i = 0; i < n && source_fade_pos < SOURCE_FADE_FRAMES; i++)
                {
                    int32_t gain = (source_fade_pos * 32768) / (SOURCE_FADE_FRAMES - 1);
                    s[i*2]   = (int16_t)((s[i*2]   * gain) >> 15);
                    s[i*2+1] = (int16_t)((s[i*2+1] * gain) >> 15);
                    source_fade_pos++;
                }
            }

            /* save last sample for fade-out if underflow occurs */
            {
                int16_t *s = (int16_t *)tx_send_buf;
                int n = frame_bytes / 4;
                source_last_sample[0] = s[(n-1)*2];
                source_last_sample[1] = s[(n-1)*2+1];
            }

            usb_drv_send_nonblocking(EP_ISO_SOURCE_IN, tx_send_buf, frame_bytes);
        }
        else
        {
            /* underflow: fade out from last sample to zero, then
             * re-enter pre-buffering to rebuild cushion */
            int16_t *s = (int16_t *)tx_send_buf;
            int n = frame_bytes / 4;
            int i;
            for (i = 0; i < n; i++)
            {
                int32_t gain = ((n - 1 - i) * 32768) / (n - 1);
                s[i*2]   = (int16_t)((source_last_sample[0] * gain) >> 15);
                s[i*2+1] = (int16_t)((source_last_sample[1] * gain) >> 15);
            }
            source_last_sample[0] = 0;
            source_last_sample[1] = 0;
            tx_read_pos = tx_write_pos; /* discard stale partial data */
            source_prebuffering = true;
            usb_drv_send_nonblocking(EP_ISO_SOURCE_IN, tx_send_buf, frame_bytes);
        }
        retval = true;
    }

    /* Feedback logic — only for playback (sink) mode.
     * In source mode, EP_ISO_FEEDBACK_IN is unallocated (ep=0)
     * and sending on it would corrupt the control endpoint. */
    if (usb_as_playback_intf_alt == 1)
    {
        /* send feedback value every N frames! */
        /* NOTE: important that we need to queue this up _the frame before_ it's needed - on MacOS especially! */
        if ((usb_drv_get_frame_number()+1) % FEEDBACK_UPDATE_RATE_FRAMES == 0 && send_fb)
        {
            if (!sent_fb_this_frame)
            {
                /* NOTE: the division of frequency must be staged to avoid overflow of 16-bit signed int
                 * as well as truncating the result to ones place!
                 * Must avoid values > 32,768 (2^15)
                 * Largest value: 192,000 --> /10: 19,200 --> /100: 192
                 * Smallest value: 44,100 --> /10: 4,410 --> /100: 44.1
                 */
                int32_t samples_base = TO_16DOT16_FIXEDPT(hw_freq_sampr[as_playback_freq_idx]/10)/100;
                int32_t buffers_filled = 0;

                if (buffers_filled_avgcount != 0)
                {
                    buffers_filled = TO_16DOT16_FIXEDPT((int32_t)buffers_filled_accumulator) / buffers_filled_avgcount;
                }
                buffers_filled_accumulator = buffers_filled_accumulator - buffers_filled_accumulator_old;
                buffers_filled_avgcount = buffers_filled_avgcount - buffers_filled_avgcount_old;
                buffers_filled_accumulator_old = buffers_filled_accumulator;
                buffers_filled_avgcount_old = buffers_filled_avgcount;

                /* someone who has implemented actual PID before might be able to do this correctly, */
                /* but this seems to work good enough? */
                /* Coefficients were 1, 0.25, 0.025 in float math --> 1, /4, /40 in fixed-point math */
                samples_fb = samples_base - (buffers_filled/4) + ((buffers_filled_old - buffers_filled)/40);
                buffers_filled_old = buffers_filled;

                /* must limit to +/- 1 sample from nominal */
                samples_fb = samples_fb > (samples_base + TO_16DOT16_FIXEDPT(1)) ? samples_base + TO_16DOT16_FIXEDPT(1) : samples_fb;
                samples_fb = samples_fb < (samples_base - TO_16DOT16_FIXEDPT(1)) ? samples_base - TO_16DOT16_FIXEDPT(1) : samples_fb;

                encodeFBfixedpt(sendFf, samples_fb, usb_drv_port_speed());
                logf("usbaudio: frame %d fbval 0x%02X%02X%02X%02X", usb_drv_get_frame_number(), sendFf[3], sendFf[2], sendFf[1], sendFf[0]);
                usb_drv_send_nonblocking(EP_ISO_FEEDBACK_IN, sendFf, usb_drv_port_speed()?4:3);

                /* debug screen counters */

                /* samples_received NOTE: need some "division staging" to not overflow signed 16-bit value */
                /* samples / (feedback frames * 2) --> samples/2 */
                /* samples_report / (2ch * 2bytes per sample) --> samples/4 */
                /* total: samples/8 */
                samples_received_report = TO_16DOT16_FIXEDPT(samples_received/8) / FEEDBACK_UPDATE_RATE_FRAMES;
                samples_received = samples_received - samples_received_last;
                samples_received_last = samples_received;
                buffers_filled_max_last = buffers_filled_max;
                buffers_filled_max = -1;
                buffers_filled_min_last = buffers_filled_min;
                buffers_filled_min = -1;
            }
            sent_fb_this_frame = true;
        }
        else
        {
            sent_fb_this_frame = false;
            if (!send_fb)
            {
                /* arbitrary wait during startup */
                if (usb_drv_get_frame_number() == (fb_startframe + (FEEDBACK_UPDATE_RATE_FRAMES*2))%(USB_FRAME_MAX+1))
                {
                    send_fb = true;
                }
            }
            buffers_filled_accumulator = buffers_filled_accumulator + (usb_audio_get_prebuffering() - MINIMUM_BUFFERS_QUEUED);
            buffers_filled_avgcount++;
            if (usb_audio_get_prebuffering() < buffers_filled_min || buffers_filled_min == -1)
            {
                buffers_filled_min = usb_audio_get_prebuffering();
            } else if (usb_audio_get_prebuffering() > buffers_filled_max)
            {
                buffers_filled_max = usb_audio_get_prebuffering();
            }
        }
    }

    return retval;
}
