/***************************************************************************
 * iAP protocol test harness -- shared declarations.
 *
 * Host-only. Built by apps/iap/test/Makefile, never by a firmware build.
 ****************************************************************************/
#ifndef IAP_TEST_H
#define IAP_TEST_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The target's volume range, spelled out here rather than read from
 * sound_min()/sound_max() so that test_volume_range_matches_the_codec
 * is a real comparison against the codec header and not a tautology.
 *
 *   ipod6g     firmware/export/cs42l55.h:29
 *              AUDIOHW_SETTING(VOLUME, "dB", 0, 1, -60, 12, -25)
 *   ipodvideo  firmware/export/wm8758.h:29
 *              AUDIOHW_SETTING(VOLUME, "dB", 0, 1, -90,  6, -25)
 */
#ifdef IPOD_VIDEO
#define IAP_TEST_VOLUME_MIN (-90)
#define IAP_TEST_VOLUME_MAX (6)
#define IAP_TEST_TARGET_NAME "ipodvideo"
#else
#define IAP_TEST_VOLUME_MIN (-60)
#define IAP_TEST_VOLUME_MAX (12)
#define IAP_TEST_TARGET_NAME "ipod6g"
#endif

/* The full dB span, and two points inside it the volume-limit cases use:
 * a limit halfway up, and a level halfway to that limit. Derived rather
 * than written out, so both targets exercise the same ratios. */
/* audiohw_set_volume() is handed centibels, not the decibels the rest of
 * the volume path uses: firmware/sound.c's prescaler path converts with
 * sound_value_to_cb(), and both codecs declare zero decimal places, so
 * the factor is ten. test_volume_codec_units_are_centibels pins that
 * against the real conversion rather than trusting this comment. */
#define IAP_TEST_CB(db) ((db) * 10)

#define IAP_TEST_VOLUME_SPAN (IAP_TEST_VOLUME_MAX - IAP_TEST_VOLUME_MIN)
#define IAP_TEST_VOLUME_MID  (IAP_TEST_VOLUME_MIN + IAP_TEST_VOLUME_SPAN / 2)
#define IAP_TEST_VOLUME_QTR  (IAP_TEST_VOLUME_MIN + IAP_TEST_VOLUME_SPAN / 4)

/* ------------------------------------------------------------------ */
/* Side effects recorded by the Rockbox stubs                           */
/* ------------------------------------------------------------------ */
struct rbstub_calls {
    int pause, resume, stop, skip, ff_rewind, reload;
    int next_dir, prev_dir;
    /* The last audio_ff_rewind() offset, in ms. */
    long last_ff_rewind;
    int last_skip;
    int randomise, sort, play_playlist;
    /* Its own counter. play_playlist counts playlist_create(), and
     * sharing one made "the browser entry point was not called"
     * indistinguishable from "a playlist was loaded". */
    int ft_play_playlist;
    /* Call order, not just counts. PlayCurrentSelection has to select
     * the track before it shuffles around it, and two call counts
     * cannot tell the two orders apart. Each of the three bumps `seq`
     * and stamps its own field. */
    int seq, seq_start, seq_randomise, seq_sort;
    int settings_save, queue_post, button_post, panics;
    long last_button_event;
    int artwork_decodes;
    int playlist_start, last_start_index;
    /* setvol() is what actually reaches the codec. codec_volume is the
     * level it applied, or -9999 if it was never called. */
    int setvol, sound_set_volume, codec_volume;
    /* The last interval passed to timeout_register(), in ticks, and how
     * many times it was called. */
    int timeouts, timeout_ticks;
    /* The rate the iAP layer moved the mixer to, or 0 if it did not. */
    int mixer_frequency;
    long last_event;
    intptr_t last_event_data;
};
extern struct rbstub_calls rbstub_calls;

void rbstub_reset(void);
void rbstub_reset_calls(void);

/* Run the timeout callback the iAP layer registered, and return the
 * interval it asks for next, in ticks. -1 if none is registered. */
int rbstub_run_timeout(void);
void rbstub_set_volume(int vol);
void rbstub_set_audio_status(int status);
void rbstub_set_playlist(int amount, int index);
void rbstub_set_playlist_first_index(int index);
/* Make playlist_get_track_info() fail the way a bad control-file read
 * does on hardware, which no index check upstream can anticipate. */
void rbstub_fail_track_info(bool fail);
/* Make get_metadata() fail after clearing the entry. */
void rbstub_fail_metadata(bool fail);
/* And a catalogue playlist that cannot be loaded. */
void rbstub_fail_playlist_create(bool fail);
/* The playlist catalogue lingo 4 browses with opendir()/readdir().
 * Empty by default, which is what made every Playlist-category
 * SelectDBRecord refuse before reaching the code under test. Only
 * names ending .m3u or .m3u8 are counted, as in iap-lingo4.c. */
void rbstub_set_playlist_catalog(const char *const *names, int n);
void rbstub_set_usb_audio_active(bool on);
/* The second, independent flag: usb_audio.c sets active and streaming
 * fourteen lines apart, and the iAP layer reads them at different
 * sites. */
void rbstub_set_usb_audio_streaming(bool on);
/* Declared unconditionally: this header is parsed before config.h, so
 * a guard on HAVE_IAP_ACCESSORY_POLL here would never be true. The
 * definition in rb_stubs.c is guarded. */
extern int rbstub_accessory_polls;
void rbstub_set_mixer_frequency(unsigned int f);
void rbstub_set_date(int year_ad, int mon, int mday, int hour, int min);
void rbstub_set_battery(int pct);
/* The charger IC's charging line, which is what the power-state reply
 * asks -- not charge_state, which is derived from charger_input_state. */
void rbstub_set_charging(bool on);
void rbstub_set_hold(bool on);
void rbstub_set_artwork(bool present);
void rbstub_fail_artwork_decode(bool fail);

/* True while the harness is modelling interrupt context. The blocking
 * stubs fail the case if they are reached with it set. */
extern bool iaptest_irq_context;
struct mp3entry *rbstub_id3(void);

/* Fire a Rockbox event the iAP layer subscribed to via add_event().
 * Returns false if nothing was subscribed for that id. */
bool rbstub_fire_event(unsigned short id, void *data);

/* ------------------------------------------------------------------ */
/* RDS pushes recorded by the tuner stubs (ipodvideo only)              */
/* ------------------------------------------------------------------ */
#define RBSTUB_RDS_MAX 8

struct rbstub_rds_push {
    int    info_id;
    size_t size;      /* what the driver asked to push */
    size_t copied;    /* what fitted in data[] */
    unsigned char data[64];
};

struct rbstub_rds {
    int n, inits, resets;
    struct rbstub_rds_push push[RBSTUB_RDS_MAX];
};
extern struct rbstub_rds rbstub_rds;

/* ------------------------------------------------------------------ */
/* Captured transmissions                                              */
/* ------------------------------------------------------------------ */
#define IAPTEST_MAX_TX      64
#define IAPTEST_MAX_TXLEN  600

struct iaptest_pkt {
    unsigned char raw[IAPTEST_MAX_TXLEN];  /* FF 55 .. checksum */
    int           rawlen;
    const unsigned char *payload;          /* lingo byte onwards */
    int           paylen;                  /* excludes the checksum */
    bool          checksum_ok;
    bool          length_form_ok;          /* short/long form per MFi 2.5.2 */
};

/* Reset all state and bring the iAP layer up. Call at the top of a test. */
void iaptest_init(void);

/* Feed one framed packet in, then run the packet handler. The harness
 * builds the FF 55 header, length field and checksum, so callers pass
 * the payload only (lingo, command, transID if any, parameters). */
void iaptest_rx(const unsigned char *payload, int len);

/* Packets iaptest_rx() handed to a firmware that was still waiting for
 * a button to go out, and so re-queued rather than handled. The runner
 * requires this to stay still across a case: a case that moves it is
 * asserting against a packet the handler never saw. Drain with
 * iaptest_button_sample() before sending. */
extern int iaptest_deferred_rx;

/* Count n verifications made without CHECK -- see iap_test.c. Anything
 * that judges and reports through iaptest_fail() must call this, or the
 * zero-assertion guard cannot see it. */
void iaptest_checked(int n);

/* Convenience: same, from a brace-enclosed byte list. */
#define IAPTEST_RX(...) do { \
        static const unsigned char _p[] = { __VA_ARGS__ }; \
        iaptest_rx(_p, (int)sizeof(_p)); \
    } while (0)

int  iaptest_tx_count(void);
const struct iaptest_pkt *iaptest_tx(int index);
void iaptest_tx_clear(void);

/* Walk a full-featured accessory through IDPS and clear captured output. */
void iaptest_enter_idps(void);

/* Stand in for the 100 Hz button tick, which is the only thing that
 * drains iap_repeatbtn. iap_handlepkt() defers every incoming packet
 * while that counter is set, so a test sending back-to-back button
 * commands has to sample in between. */
void iaptest_button_sample(int times);

/* Detach the accessory model for a case that builds raw probes the
 * model cannot follow. Says what it means at the call site. */
void iaptest_detach_model_for_raw_probes(void);

/* Force the auth state machine past the point where CHECKAUTH passes,
 * without replaying a full certificate exchange. */
void iaptest_force_authenticated(void);

/* Same, but stops before StartIDPS: a legacy accessory that identifies
 * with IdentifyDeviceLingoes and never enables transaction IDs. */
void iaptest_identify_legacy(uint32_t lingo_mask);

/* IDPS, authentication and Extended Interface mode, with the button
 * that mode entry raises already drained. See the definition. */
void iaptest_session_extended(void);

/* ------------------------------------------------------------------ */
/* Assertions                                                          */
/* ------------------------------------------------------------------ */
extern int  iaptest_failures;
extern int  iaptest_checks;

/* Only CHECK and CHECK_EQ_INT bump this. iaptest_checks counts those
 * plus the automatic per-frame and per-judgement verifications, and
 * those fire for any case that transmits at all -- so the runner's
 * "case asserted nothing" guard, which used iaptest_checks, was
 * satisfied by a case whose body was one IAPTEST_RX and no CHECK. It
 * uses this instead: a case has to say something itself. */
extern int  iaptest_asserts;
extern const char *iaptest_current;

void iaptest_fail(const char *file, int line, const char *fmt, ...);
void iaptest_hexdump(const char *label, const unsigned char *b, int len);

#define CHECK(cond, ...) do { \
        iaptest_checks++; iaptest_asserts++; \
        if (!(cond)) iaptest_fail(__FILE__, __LINE__, __VA_ARGS__); \
    } while (0)

#define CHECK_EQ_INT(got, want, what) do { \
        iaptest_checks++; iaptest_asserts++; \
        long _g = (long)(got), _w = (long)(want); \
        if (_g != _w) \
            iaptest_fail(__FILE__, __LINE__, \
                "%s: got %ld (0x%lX), want %ld (0x%lX)", \
                (what), _g, (unsigned long)_g, _w, (unsigned long)_w); \
    } while (0)

/* Assert the payload of captured packet `idx` equals the given bytes. */
void iaptest_expect_payload(const char *file, int line, int idx,
                            const unsigned char *want, int wantlen);

#define EXPECT_PAYLOAD(idx, ...) do { \
        static const unsigned char _w[] = { __VA_ARGS__ }; \
        iaptest_expect_payload(__FILE__, __LINE__, (idx), _w, (int)sizeof(_w)); \
    } while (0)

/* Test registration */
struct iaptest_case {
    const char *name;
    void (*fn)(void);
};
extern const struct iaptest_case iaptest_cases[];
extern const int iaptest_case_count;

#endif /* IAP_TEST_H */
