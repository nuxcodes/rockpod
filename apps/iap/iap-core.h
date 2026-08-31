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
#ifndef _IAP_CORE_H
#define _IAP_CORE_H

#include <stdint.h>
#include <string.h>
#include "timefuncs.h"
#include "metadata.h"
#include "playlist.h"
#include "iap.h"

/* #define LOGF_ENABLE */
#undef LOGF_ENABLE
#ifdef LOGF_ENABLE
  #include "logf.h"
#endif

/* The Model ID of the iPod we emulate. Currently a 160GB classic */
#define IAP_IPOD_MODEL (0x00130200U)
#define IAP_IPOD_VARIANT "MC293"

/* The firmware version we emulate. Currently 2.0.3 */
#define IAP_IPOD_FIRMWARE_MAJOR (2)
#define IAP_IPOD_FIRMWARE_MINOR (0)
#define IAP_IPOD_FIRMWARE_REV   (3)

/* Status code for IAP ack messages */
#define IAP_ACK_OK          (0x00)  /* Success */
#define IAP_ACK_UNKNOWN_DB  (0x01)  /* Unknown Database Category */
#define IAP_ACK_CMD_FAILED  (0x02)  /* Command failed */
#define IAP_ACK_NO_RESOURCE (0x03)  /* Out of resources */
#define IAP_ACK_BAD_PARAM   (0x04)  /* Bad parameter */
#define IAP_ACK_UNKNOWN_ID  (0x05)  /* Unknown ID */
#define IAP_ACK_PENDING     (0x06)  /* Command pending */
#define IAP_ACK_NO_AUTHEN   (0x07)  /* Not authenticated */
#define IAP_ACK_BAD_AUTHEN  (0x08)  /* Bad authentication version */
/* 0x09 reserved */
#define IAP_ACK_CERT_INVAL  (0x0A)  /* Certificate invalid */
#define IAP_ACK_CERT_PERM   (0x0B)  /* Certificate permissions invalid */
/* 0x0C-0x10 reserved */
#define IAP_ACK_RES_INVAL   (0x11)  /* Invalid accessory resistor value */

/* Add a button to the remote button bitfield. Also set iap_repeatbtn=1
 * to ensure a button press is at least delivered once.
 */
#define REMOTE_BUTTON(x) do { \
        iap_remotebtn |= (x); \
        iap_timeoutbtn = 3; \
        iap_repeatbtn = 2; \
        } while(0)

/* States of the extended command support */
enum interface_state {
    IST_STANDARD,    /* General state, support lingo 0x00 commands */
    IST_EXTENDED,   /* Extended Interface lingo (0x04) negotiated */
};

/* States of the authentication state machine */
enum authen_state {
    AUST_NONE,            /* Initial state, no message sent */
    AUST_INIT,            /* Remote side has requested authentication */
    AUST_CERTREQ,         /* Remote certificate requested */
    AUST_CERTBEG,         /* Certificate is being received */
    AUST_CERTALLRECEIVED, /* Certificate all Received  */
    AUST_CERTDONE,        /* Certificate all Done */
    AUST_CHASENT,         /* Challenge sent */
    AUST_CHADONE,         /* Challenge response received */
    AUST_AUTH,            /* Authentication complete */
};

/* State of authentication */
struct auth_t {
    enum authen_state state;        /* Current state of authentication */
    unsigned char max_section;      /* The maximum number of certificate sections */
    unsigned char next_section;     /* The next expected section number */
    uint16_t version;               /* Authentication version */
    /* When the accessory's answer stops being on time, or 0 for "not
     * waiting". MFi 2.4.2 (p.104): the accessory "must respond by
     * transmitting its entire RetAccessoryAuthenticationInfo command
     * within 1.00 seconds for Authentication 1.0 or 2.00 seconds for
     * Authentication 2.0, including the transmission of all sections
     * of its certificate", and its signature "within 7.00 seconds for
     * Authentication 1.0, 75.00 seconds for Authentication 2.0A, or
     * 2.00 seconds for Authentication 2.0B and 2.0C". The same
     * paragraph says the device "starts both a timer and a retry
     * counter to guarantee that the authentication process will
     * conclude"; this is the timer. */
    long deadline;
    bool idps;                      /* Auth initiated via IDPS (transIDs required) */
    bool idps_started;              /* StartIDPS seen. MFi 2.6 makes transaction
                                     * IDs mandatory "beginning with and
                                     * including the StartIDPS command", which
                                     * is well before idps is set at EndIDPS. */
    uint16_t ipod_tid;              /* one ID for the whole handshake */
    bool ipod_tid_valid;
    uint8_t tid_hi;                 /* Last transaction ID from accessory */
    uint8_t tid_lo;
};

/* State of GetAccessoryInfo */
enum accinfo_state {
    ACCST_NONE,     /* Initial state, no message sent */
    ACCST_INIT,     /* Send out initial GetAccessoryInfo */
    ACCST_SENT,     /* Wait for initial RetAccessoryInfo */
    ACCST_DATA,     /* Query device information, according to capabilities */
};

/* A struct describing an attached device and it's current
 * state
 */
struct device_t {
    struct auth_t auth;             /* Authentication state */
    enum accinfo_state accinfo;     /* Accessory information state */
    uint32_t lingoes;               /* Negotiated lingoes */
    uint32_t notifications;         /* Requested notifications. These are just the
                                     * notifications explicitly requested by the
                                     * device
                                     */
    uint32_t changed_notifications; /* Tracks notifications that changed since the last
                                     * call to SetRemoteEventNotification or GetRemoteEventStatus
                                     */
    bool do_notify;                 /* Display Remote notifications on */
    /* Extended Interface play-status notifications, the four-byte mask
     * of MFi Table 5-45 (p.425). Kept apart from do_notify because the
     * two lingoes subscribe independently: collapsing both into one
     * bool let either turn the other off. */
    uint32_t pb_notifications;
    /* Whether the power and battery state has been reported to this
     * accessory yet, in the sense volume_reported has for the volume.
     * Without it an accessory that subscribes to Table 4-59 (p.255)
     * bit 5 hears nothing until the battery moves, and a replacement
     * accessory hears nothing at all -- the fields it is compared
     * against outlive the session that filled them. */
    bool power_reported;
    /* Last track position reported through type 0x04, for change
     * detection. */
    unsigned long pb_trackpos_ms;
    uint32_t      pb_trackpos_s;    /* Table 5-45 bit 04, type 0x07 */
    uint32_t      pb_chapter_index;
    uint32_t      pb_chapterpos_ms;
    uint32_t      pb_chapterpos_s;
    uint32_t      pb_numtracks;     /* Table 5-45 bit 12, type 0x0E */
    uint32_t      numtracks;        /* Table 4-59 bit 18, event 0x12.
                                     * Its own shadow, not pb_numtracks:
                                     * two lingoes sharing one is the
                                     * bug that was fixed for
                                     * track_index and chapter_index. */
    unsigned char pb_play_status;   /* for Extended Interface bit 00 */
    unsigned char pb_ext_state;     /* for bit 01: iap_play_state_byte() */
    bool contents_dirty;            /* the queue was reordered; Table 5-45
                                     * bit 12 and Table 4-59 bit 18 both
                                     * owe an event that a track count
                                     * comparison cannot see */
    bool pb_track_changed;          /* a track change is owed to bit 02 */
    unsigned char pb_seeking;       /* 0x02 FFW or 0x03 REW in progress,
                                     * for the seek-stop notification */
    bool do_power_notify;           /* Whether to send power change notifications.
                                     * These are sent automatically to all devices
                                     * that used IdentifyDeviceLingoes to identify
                                     * themselves, independent of other notifications
                                     */

    uint32_t trackpos_ms;           /* These fields are to save the current state */
    uint32_t track_index;           /* of various fields so we can send a notification */
    uint32_t chapter_index;         /* if they change */
    uint32_t chapter_track_index;
    uint16_t chapter_count;
    unsigned char play_status;
    bool mute;
    unsigned char volume;           /* last level sent, in the 0..255
                                     * protocol form, not dB */
    bool volume_reported;           /* a level has been sent this session */
    unsigned char power_state;
    unsigned char battery_level;
    uint32_t equalizer_index;
    unsigned char shuffle;
    unsigned char repeat;
    struct tm datetime;
    unsigned char alarm_state;
    unsigned char alarm_hour;
    unsigned char alarm_minute;
    unsigned char backlight;
    bool hold;
    unsigned char soundcheck;
    unsigned char audiobook;
    uint16_t trackpos_s;
    uint32_t capabilities;          /* Capabilities of the device, as returned by type 0
                                     * of GetAccessoryInfo
                                     */
    uint32_t capabilities_queried;  /* Capabilities already queried */
    bool audio_init_pending;        /* Send GetAccSampleRateCaps after auth */
    bool audio_attrs_pending;       /* Resend TrackNewAudioAttributes */
    bool tuner_caps_pending;        /* Send GetTunerCaps once authenticated */
    bool poll_baseline_pending;     /* Take the poll baseline in thread ctx */
    bool unmute_pending;            /* Lift an accessory's mute, in thread ctx */
    /* RetAccessoryInfo type 0x09, Table 3-46 (p.147) and Table 3-53
     * (p.151): "the maximum size of a packet from the Apple device that
     * the accessory can support". p.150 sets the default when the
     * accessory returns nothing at 1024 and the legal declared range at
     * 128..65529, so an accessory only bothers to declare a value when
     * it is below the default -- which is exactly the case this device
     * used to ignore. */
    uint16_t acc_max_payload;
    uint32_t idps_lingoes;          /* Lingoes parsed from IDPS IdentifyToken */
    uint32_t idps_options;          /* Options from IDPS IdentifyToken */
    uint32_t idps_deviceid;         /* DeviceID from IDPS IdentifyToken */
    uint16_t ipod_trans_id;         /* Transaction ID for iPod-originated cmds */
};

extern struct device_t device;
/* MFi spec 2.4.2: "All lingo-authenticated commands and features are
 * available to accessories once the Apple device has sent the accessory
 * an AckAccessoryAuthenticationInfo command with success status (0x00).
 * This provisional authentication lasts until the Apple device sends a
 * AckAccessoryAuthenticationStatus command indicating that
 * authentication has finished."
 *
 * That ack is sent as the state moves to AUST_CERTDONE, and the states
 * from there to AUST_AUTH are the background half of the handshake, so
 * treat the accessory as authenticated throughout. Requiring AUST_AUTH
 * rejected commands the accessory is entitled to send during that
 * window -- SetUIMode among them, which the spec's own worked example
 * (Table 2-13) shows being accepted there.
 */
/* Transaction IDs are in force. MFi 2.6 (p.110) makes them mandatory
 * "beginning with and including the StartIDPS command", and the
 * IMPORTANT note on p.95 repeats it "regardless of lingo". device.auth.idps
 * alone is not this test: it is only set at EndIDPS, so using it left the
 * whole handshake window parsing payloads two bytes early. That mistake
 * has been shipped twice; use this instead. */
#define DEVICE_TRANSID_ACTIVE (device.auth.idps || device.auth.idps_started)

#define DEVICE_AUTHENTICATED (device.auth.state >= AUST_CERTDONE)
#define DEVICE_AUTH_RUNNING ((device.auth.state != AUST_NONE) && (device.auth.state != AUST_AUTH))
#define DEVICE_LINGO_SUPPORTED(x) (device.lingoes & BIT_N((x)&0x1f))

extern unsigned long iap_remotebtn;
extern unsigned int iap_timeoutbtn;
extern int iap_repeatbtn;

extern unsigned char* iap_txpayload;
extern unsigned char* iap_txnext;

/* These are a number of helper macros to manage the dynamic TX buffer content
 * These macros DO NOT CHECK for buffer overflow. iap_send_tx() will, but
 * it might be too late at that point. See the current size of TX_BUFLEN
 */

/* Initialize the TX buffer with a lingo and command ID. This will reset the
 * data pointer, effectively invalidating unsent information in the TX buffer.
 * There are two versions of this, one for 1 byte command IDs (all Lingoes except
 * 0x04) and one for two byte command IDs (Lingo 0x04)
 */
#define IAP_TX_INIT(lingo, command) do { \
        iap_txnext = iap_txpayload; \
        IAP_TX_PUT((lingo)); \
        IAP_TX_PUT((command)); \
        } while (0)

#define IAP_TX_INIT4(lingo, command) do { \
        iap_txnext = iap_txpayload; \
        IAP_TX_PUT((lingo)); \
        IAP_TX_PUT_U16((command)); \
        } while (0)

/* Put an unsigned char into the TX buffer */
#define IAP_TX_PUT(data) *(iap_txnext++) = (data)

/* Put a 16bit unsigned quantity into the TX buffer */
#define IAP_TX_PUT_U16(data) do { \
        put_u16(iap_txnext, (data)); \
        iap_txnext += 2; \
        } while (0)

/* Put a 32bit unsigned quantity into the TX buffer */
#define IAP_TX_PUT_U32(data) do { \
        put_u32(iap_txnext, (data)); \
        iap_txnext += 4; \
        } while (0)

/* Put an arbitrary amount of data (identified by a char pointer and
 * a length) into the TX buffer
 */
#define IAP_TX_PUT_DATA(data, len) do { \
        memcpy(iap_txnext, (unsigned char *)(data), (len)); \
        iap_txnext += (len); \
        } while(0)

/* Put a NULL terminated string into the TX buffer, including the
 * NULL byte
 */
#define IAP_TX_PUT_STRING(str) IAP_TX_PUT_DATA((str), strlen((str))+1)

/* Put a NULL terminated string into the TX buffer, taking care not to
 * overflow the buffer. If the string does not fit into the TX buffer
 * it will be truncated, but always NULL terminated.
 *
 * This function is expensive compared to the other IAP_TX_PUT_*
 * functions
 */
#define IAP_TX_PUT_STRLCPY(str) iap_tx_strlcpy(str)

/* Put an iPod-originated transaction ID into the TX buffer if in IDPS mode.
 * Increments the counter after use.
 */
#define IAP_TX_PUT_IPOD_TRANSID() do { \
        if (DEVICE_TRANSID_ACTIVE) { \
            IAP_TX_PUT((device.ipod_trans_id >> 8) & 0xFF); \
            IAP_TX_PUT(device.ipod_trans_id & 0xFF); \
            device.ipod_trans_id++; \
        } \
    } while(0)

/* Reuse one transaction ID for the complete authentication exchange. */
#define IAP_TX_PUT_IPOD_AUTH_TRANSID() do { \
        if (DEVICE_TRANSID_ACTIVE) { \
            if (!device.auth.ipod_tid_valid) { \
                device.auth.ipod_tid = device.ipod_trans_id++; \
                device.auth.ipod_tid_valid = true; \
            } \
            IAP_TX_PUT((device.auth.ipod_tid >> 8) & 0xFF); \
            IAP_TX_PUT(device.auth.ipod_tid & 0xFF); \
        } \
    } while(0)

extern unsigned char lingo_versions[32][2];
#define LINGO_SUPPORTED(x) (LINGO_MAJOR((x)&0x1f) > 0)
#define LINGO_MAJOR(x) (lingo_versions[(x)&0x1f][0])
#define LINGO_MINOR(x) (lingo_versions[(x)&0x1f][1])

void put_u16(unsigned char *buf, const uint16_t data);
void put_u32(unsigned char *buf, const uint32_t data);
uint32_t get_u32(const unsigned char *buf);
uint16_t get_u16(const unsigned char *buf);
void iap_tx_strlcpy(const char *str);

void iap_reset_auth(struct auth_t* auth);
void iap_reset_device(struct device_t* device);

/* The bitmask GetRemoteEventStatus answers with, and the read clears it
 * (MFi 4.3.13, p.263). Detects changes itself rather than relying on
 * the notification path, which only runs for subscribed events. */
uint32_t iap_take_changed_events(void);

/* Clear the per-accessory state iap-lingo2.c keeps outside device_t.
 * Called from iap_reset_device(). */
void iap_reset_lingo2(void);
void iap_wake(void);
void iap_send_pb_extended(unsigned char state);
void iap_send_audio_attrs(void);

/* Remote-button hold times, in iap_periodic() ticks (10 Hz). Shared
 * because two lingoes start a seek: Extended Interface's PlayControl
 * and Display Remote's SetiPodStateInfo play status. */
#define IAP_BTN_TAP   3     /* ~300ms at the 10Hz tick */
#define IAP_BTN_HELD  100   /* ~10s safety bound */
bool iap_seek_start(bool forward);
void iap_seek_stop(void);
void iap_reset_lingo1(void);
void iap_lingo1_send_pending(void);
void iap_reset_lingo4(void);

/* What a caller means about Restore on Exit.
 *
 * KEEP exists because a physical button is not an accessory request:
 * the Simple Remote shuffle and repeat buttons used to pass "no", which
 * cleared a latch the accessory had armed -- so a user pressing Shuffle
 * mid-session disarmed the restore and wrote the accessory's temporary
 * value to disk. */
enum iap_restore {
    IAP_RESTORE_KEEP = -1,      /* not an accessory request */
    IAP_RESTORE_NO   = 0,       /* accessory asked for it to persist */
    IAP_RESTORE_YES  = 1,       /* accessory asked for it back on detach */
};

void iap_shuffle_state(bool state, enum iap_restore restore);
void iap_repeat_state(unsigned char state, enum iap_restore restore);
void iap_volume_restore_arm(const enum iap_restore restore);
void iap_arm_settings_restore(void);
void iap_restore_settings(void);
void iap_repeat_next(void);
void iap_fill_power_state(void);

void iap_send_tx(void);

/* Codec volume <-> the protocol's 0..255 UI volume (MFi Table 4-61,
 * event 0x04). The range comes from sound_min/sound_max at runtime, so
 * this is correct on both the CS42L55 and the WM8758. */
unsigned char iap_volume_to_byte(int volume);
unsigned char iap_volume_to_ui_byte(int volume);
int iap_byte_to_volume(unsigned char level);
int iap_byte_to_abs_volume(unsigned char level);

extern enum interface_state interface_state;
void iap_interface_state_change(const enum interface_state new);

extern bool iap_btnrepeat;
extern bool iap_btnshuffle;
extern bool iap_btnstop;
extern bool iap_btnalbum;    /* Next/Previous Album, Table 4-14 p.227 */
extern bool iap_btnchapter;

/* Accessory Power lingo, Table C-37 (p.548). Public so iap-lingo0.c
 * can arm it when an accessory negotiates the lingo. */
void iap_high_power_arm(void);
#if CONFIG_TUNER
extern bool iap_btnradiomute;
#endif

uint32_t iap_get_trackpos(void);
int iap_current_track_index(void);
uint32_t iap_get_trackindex(void);

/* Between the accessory's index space and Rockbox's. Both wrap at the
 * end of the queue; see the definitions. */
/* Playback state as Table 4-62 (p.262) encodes it. */
unsigned char iap_play_state_byte(void);
unsigned char iap_play_state_reported(void);

uint32_t iap_track_to_mfi(long rb_index);
long     iap_track_from_mfi(uint32_t mfi_index);
/* Events in the iap_queue. Here rather than in iap-core.c so a test
 * can say which one iap_wake() posted. */
#define IAP_EV_TICK         (1)     /* The regular task timeout */
#define IAP_EV_MSG_RCVD     (2)     /* A complete message has been received from the device */
#define IAP_EV_MALLOC       (3)     /* Allocate memory for the RX/TX buffers */
#define IAP_EV_ARTWORK      (4)

bool iap_get_trackinfo(const unsigned int track, struct mp3entry* id3);
void iap_schedule_artwork(uint32_t transfer_id);

struct iap_chapter_info {
    uint32_t index;
    uint32_t count;
    uint32_t offset_ms;
    uint32_t length_ms;
    uint32_t elapsed_ms;
    const char *name;
};

bool iap_chapter_at(const struct mp3entry *id3, uint32_t index,
                    struct iap_chapter_info *chapter);
bool iap_current_chapter(const struct mp3entry *id3,
                         struct iap_chapter_info *chapter);
bool iap_set_chapter(uint32_t index);
bool iap_skip_chapter(int direction);
bool iap_play_or_resume(void);
void iap_set_mute(bool mute);

#endif
