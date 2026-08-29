/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Host-side stubs for the iAP protocol test harness.
 *
 * The iAP sources are compiled unmodified. Everything they call into the
 * rest of Rockbox is answered here by a controllable fake, so a test can
 * set up a playback/settings state and then assert on the exact bytes the
 * protocol layer puts on the wire.
 *
 * This file is only ever built by apps/iap/test/Makefile. It is not part
 * of any firmware build.
 *
 ****************************************************************************/

#include "iap_test.h"
#include "iap-core.h"   /* iap_txnext/iap_txpayload, for the guard below */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "settings.h"
#include "metadata.h"
#include "playlist.h"
#include "audio.h"
#include "button.h"
#include "kernel.h"
#include "timefuncs.h"
#include "filetree.h"
#include "dir.h"
#include "sound.h"
#include "misc.h"
#include "tuner.h"
#include "bmp.h"
#include "jpeg_load.h"
#if CONFIG_TUNER
#include "ipod_remote_tuner.h"
#endif

/* ------------------------------------------------------------------ */
/* Rockbox globals the iAP layer reads                                  */
/* ------------------------------------------------------------------ */

struct user_settings global_settings;
struct system_status global_status;

volatile long current_tick = 0;

/* ------------------------------------------------------------------ */
/* Fake playback state, driven by tests through rbstub_*()              */
/* ------------------------------------------------------------------ */

static struct mp3entry fake_id3;
static int    fake_audio_status  = 0;
/* Firmware's playlist_get_current() returns &current_playlist and is
 * never NULL, so iap_get_trackindex() dereferences it unguarded. Mirror
 * that: hand back a real object. */
static struct playlist_info fake_playlist;

/* Records of side effects, so tests can assert that a command did or did
 * not reach the playback engine. */
struct rbstub_calls rbstub_calls;

static unsigned int fake_mixer_frequency = 44100;

static int fake_battery = 75;
static bool fake_hold = false;
static bool track_info_fails = false;
static bool metadata_fails = false;
static bool playlist_create_fails = false;
static bool artwork_present = false;
static bool artwork_decode_fails = false;
void rbstub_set_usb_audio_active(bool on);
void rbstub_set_usb_audio_streaming(bool on);
void rbstub_set_charging(bool on);

#define RBSTUB_MAX_CATALOG 8
static const char *catalog[RBSTUB_MAX_CATALOG];
static int catalog_n;
static int catalog_pos;
static bool catalog_open;
static struct dirent catalog_ent;

#if CONFIG_TUNER
/* ipod_remote_tuner.c's own state. The Makefile builds that file with
 * -Dstatic= so a case can read tuner_frequency; the same flag is what
 * lets rbstub_reset() put all of it back between cases. */
extern int  tuner_frequency;
extern int  tuner_signal_power;
extern bool radio_tuned;
extern unsigned char tuner_param, old_tuner_param;
extern int  mono_mode, old_region;
#endif

void rbstub_reset(void)
{
    memset(&fake_id3, 0, sizeof(fake_id3));
    memset(&rbstub_calls, 0, sizeof(rbstub_calls));
    memset(&global_settings, 0, sizeof(global_settings));
    memset(&global_status, 0, sizeof(global_status));

    fake_id3.title  = (char *)"Test Title";
    fake_id3.artist = (char *)"Test Artist";
    fake_id3.album  = (char *)"Test Album";
    fake_id3.length = 240000;
    fake_id3.elapsed = 1000;
    fake_battery = 75;
    fake_hold = false;

    fake_audio_status = 0;
    memset(&fake_playlist, 0, sizeof(fake_playlist));
    fake_playlist.amount = 1;
    fake_playlist.index = 0;
    fake_playlist.first_index = 0;

    global_status.volume = -25;      /* the ipod6g default */

    /* volume_limit_set_default() (apps/settings_list.c:922) makes the
     * limit sound_max, so a default configuration has the UI and
     * absolute volume scales coincide. test_volume_ui_scale_follows_
     * volume_limit lowers it to check they then diverge. */
    global_settings.volume_limit = sound_max(SOUND_VOLUME);
    fake_mixer_frequency = 44100;
    rbstub_calls.codec_volume = -9999;

    /* The three that were left behind. rbstub_reset() runs before every
     * case and is what makes them independent, so anything it misses is
     * a case inheriting the one before it -- which this suite has
     * already been bitten by four times, in the firmware rather than
     * here: track_info_fails made a later case's playlist reads fail
     * for no reason it could see.
     *
     * The event table and the registered timeout are deliberately not
     * cleared, though a review asked for both. iap_setup() installs
     * them on its first call and iap_setupflag guards the rest, exactly
     * as on the target -- so wiping either would leave every case after
     * the first with no track-change handler and no tick, and the cases
     * that measure the idle tick fail immediately. Neither can grow
     * without bound: add_event() returns true for a pair it already
     * holds, and there is one timeout. */
    track_info_fails = false;
    metadata_fails = false;
    playlist_create_fails = false;
    artwork_present = false;
    artwork_decode_fails = false;
    rbstub_set_usb_audio_active(false);
    rbstub_set_usb_audio_streaming(false);
    rbstub_set_charging(false);
    catalog_n = 0;
    catalog_open = false;
#if CONFIG_TUNER
    memset(&rbstub_rds, 0, sizeof(rbstub_rds));

    /* The tuner driver's own state, all of it.
     *
     * radio_present was the only one reset, so the other five carried
     * from case to case: a case that had tuned left tuner_frequency and
     * radio_tuned set for whatever ran next, and the caches
     * rmt_tuner_region() and RADIO_FORCE_MONO compare against kept the
     * previous case's values, so a later case setting the same region
     * or mode sent nothing and the case watching for the packet passed
     * or failed on what came before it. Reordering the file turns up
     * three real failures.
     *
     * -Dstatic= is what makes them reachable from here; the same flag
     * the hid and uart suites use. */
    radio_present = 0;
    tuner_frequency = 0;
    tuner_signal_power = 0;
    radio_tuned = false;
    tuner_param = 0x00;
    old_tuner_param = 0xFF;
    mono_mode = -1;
    old_region = -1;
#endif
    current_tick = 0;
}

/* Clear only the side-effect counters, leaving the device and playlist
 * state a case has already set up. */
void rbstub_reset_calls(void)
{
    memset(&rbstub_calls, 0, sizeof(rbstub_calls));
    rbstub_calls.codec_volume = -9999;
}

void rbstub_set_volume(int vol)
{
    /* Stand in for the user turning the wheel, which goes through
     * setvol() (apps/gui/wps.c:888) and so reaches the codec. */
    global_status.volume = vol;
    setvol();
}

/* The real setvol(), apps/misc.c:871, clamped and applied. Reproduced
 * rather than compiled because misc.c pulls in most of the application.
 * The point of the stub is the last line: something has to reach the
 * codec, and the iAP path never did. */
static void no_open_packet(const char *fn);

void setvol(void)
{
    /* Reaches the codec, so it is a blocking call in both senses the
     * guard tracks -- and the detach path wants to call it. */
    no_open_packet("setvol");

    int volume = global_status.volume;

    if (volume < sound_min(SOUND_VOLUME))
        volume = sound_min(SOUND_VOLUME);
    if (volume > sound_max(SOUND_VOLUME))
        volume = sound_max(SOUND_VOLUME);
    if (volume > global_settings.volume_limit)
        volume = global_settings.volume_limit;

    rbstub_calls.setvol++;
    sound_set_volume(volume);   /* the real one, firmware/sound.c:314 */
    global_status.last_volume_change = current_tick;
}
void rbstub_set_audio_status(int st)     { fake_audio_status = st; }
void rbstub_set_playlist(int amount, int index)
{
    fake_playlist.amount = amount;
    fake_playlist.index = index;
    fake_playlist.first_index = 0;
}
struct mp3entry *rbstub_id3(void)        { return &fake_id3; }

void rbstub_set_artwork(bool present)
{
    artwork_present = present;
    strcpy(fake_id3.path, "/track000.mp3");
}

void rbstub_fail_artwork_decode(bool fail)
{
    artwork_decode_fails = fail;
}

/* ------------------------------------------------------------------ */
/* Audio / playback                                                     */
/* ------------------------------------------------------------------ */

/* Blocking calls must not be made with a packet half-built.
 *
 * Three handlers have now been fixed for this and each was found by
 * reading, not by the suite: audio_current_track() and
 * iap_get_trackinfo() in lingo 3 (077f3c2898), get_time() in the same
 * file, and playlist_next() in the periodic handler. The suite could
 * not see any of them, because every stub here returns instantly by
 * construction -- the one property the real functions do not have.
 *
 * On the target each of these yields: audio_current_track() takes
 * id3_mutex, get_time() reads the RTC over i2c once a second and takes
 * i2c_mtx, playlist_next() opens with a queue_send() to the playlist
 * thread. Yielding with the buffer open lets the UI thread run, and on
 * the 5G a tuner_set() from the radio screen reaches iap_send_pkt(),
 * which rewinds iap_txnext to the payload start. Whatever the handler
 * writes next lands inside the tuner command's frame.
 *
 * So the stubs do not block -- they check that blocking would have
 * been safe. iap_txnext is at iap_txpayload except between an
 * IAP_TX_INIT and its iap_send_tx(), which is exactly the window.
 * This turns a class that needs a reviewer into one the suite reports,
 * and it covers calls that do not exist yet.
 *
 * Everything here that yields on the target is guarded, not just the
 * three the first version covered: audio_pause/resume/stop and
 * audio_flush_and_reload_tracks go through audio_queue_send(), which is
 * a queue_send(); audio_skip takes id3_mutex; playlist_get_track_info,
 * playlist_randomise and playlist_sort take the playlist write lock and
 * reach dircache; get_metadata opens a file; settings_save writes one.
 *
 * battery_level() and button_hold() are deliberately not guarded --
 * both are plain reads on both targets, HAVE_BATTERY_SWITCH is
 * undefined so no ADC is involved, and the 6G's hold switch is a value
 * the PMU driver caches.
 */
/* Set while modelling the 6G detach IRQ so blocking stubs fail. */
bool iaptest_irq_context;

static void no_open_packet(const char *fn)
{
    if (iaptest_irq_context)
        iaptest_fail(__FILE__, __LINE__,
                     "%s() was reached from interrupt context. It blocks "
                     "or takes a mutex, and mutex_lock() "
                     "asserts CPU_MODE_THREAD_CONTEXT -- on the 6G this "
                     "panics the device at dock detach", fn);

    if (iap_txnext != iap_txpayload)
        iaptest_fail(__FILE__, __LINE__,
                     "%s() was called with a packet half-built (%d bytes "
                     "in the TX buffer). It blocks on the target, and a "
                     "yield here lets another thread rewind iap_txnext "
                     "under this one -- hoist the call above IAP_TX_INIT",
                     fn, (int)(iap_txnext - iap_txpayload));
}

struct mp3entry *audio_current_track(void)
{
    no_open_packet("audio_current_track");
    return &fake_id3;
}
int  audio_status(void)                    { return fake_audio_status; }
void audio_pause(void)
{
    no_open_packet("audio_pause");
    rbstub_calls.pause++;
    /* Paused, not stopped. This used to zero the status, and the
     * firmware asks iap_play_state_byte() straight afterwards at
     * several sites -- SelectDBRecord's Track arm, SetCurrentPlayingTrack
     * and SelectSortDBRecord all pause, then branch on whether anything
     * is playing. With the stub reporting stopped they took the
     * start-from-scratch branch in the suite and the skip branch on
     * hardware, so the arm under test was never the arm that runs.
     *
     * And a stopped engine does not become paused: playback.c's
     * audio_on_pause() early-returns on PLAY_STOPPED, which is the same
     * rule audio_resume() below already models. */
    if (fake_audio_status & AUDIO_STATUS_PLAY)
        fake_audio_status = AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE;
}
void audio_resume(void)
{
    no_open_packet("audio_resume");
    rbstub_calls.resume++;
    /* Only a paused engine resumes. playback.c's audio_on_pause()
     * early-returns when the status is PLAY_STOPPED, so setting PLAY
     * here unconditionally made every case that started from stopped
     * look as though it had worked -- which is how a resume that could
     * not resume stayed invisible. */
    if (fake_audio_status & AUDIO_STATUS_PAUSE)
        fake_audio_status = AUDIO_STATUS_PLAY;
}

void playlist_start(int start_index, unsigned long elapsed,
                    unsigned long offset)
{
    no_open_packet("playlist_start");
    (void)elapsed; (void)offset;
    rbstub_calls.playlist_start++;
    rbstub_calls.last_start_index = start_index;
    rbstub_calls.seq_start = ++rbstub_calls.seq;
    fake_playlist.index = start_index;
    fake_audio_status = AUDIO_STATUS_PLAY;
}
void audio_stop(void)
{
    no_open_packet("audio_stop");
    rbstub_calls.stop++;   fake_audio_status = 0;
}
void audio_skip(int n)
{
    no_open_packet("audio_skip");
    rbstub_calls.skip++;   rbstub_calls.last_skip = n;

    /* Two things this did not model, both of which let a handler look
     * right in the suite and be wrong on the device.
     *
     * apps/playback.c:3223 returns immediately on PLAY_STOPPED, so a
     * skip issued from a stopped engine moves nothing. The call still
     * counts -- it was made -- but it has no effect.
     *
     * And a skip that does happen moves the queue position. Leaving
     * fake_playlist.index alone meant a second skip in the same handler
     * computed its distance from a position the first one had already
     * left, which is exactly the arithmetic these commands get wrong. */
    if (!(fake_audio_status & AUDIO_STATUS_PLAY))
        return;
    if (fake_playlist.amount > 0) {
        int i = (fake_playlist.index + n) % fake_playlist.amount;
        if (i < 0)
            i += fake_playlist.amount;
        fake_playlist.index = i;
    }
}
/* apps/playback.c:4051/:4057 -- both are audio_skip(+/-1), so they
 * model the same effect through the same stub. */
void audio_next(void) { audio_skip(1); }
void audio_prev(void) { audio_skip(-1); }

/* Album skip, Table 4-14 (p.227) buttons 5 and 6. Both are a bare
 * queue_post in apps/playback.c, so they are safe from the iAP thread
 * -- but they still reach the playback engine and get the guard. */
void audio_next_dir(void)
{
    no_open_packet("audio_next_dir");
    rbstub_calls.next_dir++;
}
void audio_prev_dir(void)
{
    no_open_packet("audio_prev_dir");
    rbstub_calls.prev_dir++;
}
void audio_ff_rewind(long n)
{
    /* Guarded like the rest: this reaches the playback engine, so it
     * must not happen with a half-built packet in the TX buffer. */
    no_open_packet("audio_ff_rewind");
    rbstub_calls.ff_rewind++;

    /* The offset was discarded, and there was no field to hold it --
     * so audio_ff_rewind(0) at either Display Remote seek site, or
     * seconds passed where milliseconds are wanted, changed nothing
     * any case could see. */
    rbstub_calls.last_ff_rewind = n;

    /* apps/playback.c:3378 returns on PLAY_STOPPED, so a seek from a
     * stopped engine moves nothing. */
    if (!(fake_audio_status & AUDIO_STATUS_PLAY))
        return;
    fake_id3.elapsed = (unsigned long)n;
}
void audio_flush_and_reload_tracks(void)
{
    no_open_packet("audio_flush_and_reload_tracks");
    rbstub_calls.reload++;
}

/* ------------------------------------------------------------------ */
/* Playlist                                                             */
/* ------------------------------------------------------------------ */

int playlist_amount(void) { return fake_playlist.amount; }
/* The seed a randomise last used. apps/playlist.c writes it in
 * randomise_playlist_unlocked() and playlist_sort() leaves it alone,
 * which is what makes it a reorder signal. */
int playlist_get_seed(const struct playlist_info *pl)
{ (void)pl; return fake_playlist.seed; }
int playlist_next(int steps)
{
    /* Nothing in apps/iap calls this any more -- every site that used
     * it as a query went to iap_current_track_index(). Left as a stub
     * so the guard fires if one comes back. */
    no_open_packet("playlist_next");
    return fake_playlist.index + steps;
}
int playlist_get_first_index(const struct playlist_info *pl)
{ return pl ? pl->first_index : fake_playlist.first_index; }

/* first_index is 0 on a freshly built queue and non-zero once it has
 * been randomised or sorted (apps/playlist.c:1507 sets
 * index = first_index = i). Leaving it at 0 for every case made the
 * accessory's index space and Rockbox's identical, so a handler that
 * confused the two passed. */
void rbstub_set_playlist_first_index(int index)
{
    fake_playlist.first_index = index;
}
struct playlist_info *playlist_get_current(void) { return &fake_playlist; }
int playlist_randomise(struct playlist_info *pl, unsigned int seed, bool start)
{
    no_open_packet("playlist_randomise"); (void)pl; (void)start;
    /* The real one stores the seed it shuffled with
     * (apps/playlist.c), and playlist_get_seed() is how anything else
     * learns the order moved. */
    fake_playlist.seed = (int)seed;
    rbstub_calls.randomise++;
    rbstub_calls.seq_randomise = ++rbstub_calls.seq;
    return 0; }
int playlist_sort(struct playlist_info *pl, bool start)
{
    no_open_packet("playlist_sort"); (void)pl; (void)start;
    rbstub_calls.sort++;
    rbstub_calls.seq_sort = ++rbstub_calls.seq;
    return 0; }

/* Real playlist_get_track_info() returns 0 for an in-range index.
 * Returning -1 unconditionally made every caller bail early, so the
 * whole metadata path in lingo 3 was unreachable and the cases covering
 * it were asserting on rejections. */
/* On hardware this call also fails when the playlist control file
 * cannot be read, which no range check upstream can predict. Without a
 * way to make it fail, a handler that ignores the return is
 * indistinguishable from one that checks it. */

void rbstub_fail_track_info(bool fail)
{
    track_info_fails = fail;
}

int playlist_get_track_info(struct playlist_info *pl, int index,
                            struct playlist_track_info *info)
{
    no_open_packet("playlist_get_track_info");
    (void)pl;
    if (track_info_fails)
        return -1;
    if (index < 0 || index >= fake_playlist.amount)
        return -1;
    if (info) {
        memset(info, 0, sizeof(*info));
        info->index = index;
        /* Encode the index in the filename, so get_metadata() below can
         * give each track distinguishable data. Handing every track the
         * same metadata made it impossible for a test to tell whether a
         * handler had honoured the index it was given -- which is how a
         * handler that ignored it entirely passed. */
        snprintf(info->filename, sizeof(info->filename),
                 "/track%03d.mp3", index);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Metadata / filetree                                                  */
/* ------------------------------------------------------------------ */

/* Per-track metadata derived from the filename playlist_get_track_info()
 * built, so a test can tell which track a handler actually answered
 * about. Track n gets a length of 10000 + n and a title ending in n. */
static char meta_title[3][64];
static int  meta_slot;

void rbstub_fail_metadata(bool fail)
{
    metadata_fails = fail;
}

bool get_metadata(struct mp3entry *id3, int fd, const char *trackname)
{
    no_open_packet("get_metadata");
    int n = -1;
    (void)fd;

    if (metadata_fails) {
        memset(id3, 0, sizeof(*id3));
        return false;
    }

    memcpy(id3, &fake_id3, sizeof(*id3));

    if (trackname && sscanf(trackname, "/track%d.mp3", &n) == 1 && n >= 0) {
        char *slot = meta_title[meta_slot++ % 3];
        snprintf(slot, sizeof(meta_title[0]), "Title of track %d", n);
        id3->title  = slot;
        id3->length = 10000 + n;
    }
    return true;
}

bool find_albumart(const struct mp3entry *id3, char *buf, int buflen,
                   const struct dim *dim)
{
    no_open_packet("find_albumart");
    (void)id3;
    (void)dim;
    if (!artwork_present || buflen < 10)
        return false;
    strcpy(buf, "/dev/null");
    return true;
}

static int decode_artwork(struct bitmap *bmp, int maxsize, int format)
{
    int size;

    no_open_packet("decode_artwork");
    if (artwork_decode_fails)
        return -1;

    size = bmp->width * bmp->height * (int)sizeof(fb_data);
    if (format & FORMAT_RETURN_SIZE)
        return size;
    if (maxsize < size)
        return -1;

    fb_data *pixels = (fb_data *)bmp->data;
    for (int i = 0; i < bmp->width * bmp->height; i++)
        pixels[i] = (fb_data)(0x1000 + i);
    rbstub_calls.artwork_decodes++;
    return size;
}

int read_bmp_fd(int fd, struct bitmap *bmp, int maxsize, int format,
                const struct custom_format *cformat)
{
    (void)fd;
    (void)cformat;
    return decode_artwork(bmp, maxsize, format);
}

int read_jpeg_fd(int fd, struct bitmap *bmp, int maxsize, int format,
                 const struct custom_format *cformat)
{
    (void)fd;
    (void)cformat;
    return decode_artwork(bmp, maxsize, format);
}

int clip_jpeg_fd(int fd, int flags, unsigned long jpeg_size,
                 struct bitmap *bmp, int maxsize, int format,
                 const struct custom_format *cformat)
{
    (void)fd;
    (void)flags;
    (void)jpeg_size;
    (void)cformat;
    return decode_artwork(bmp, maxsize, format);
}

/* Signature taken from apps/filetree.h, which is now included so the
 * compiler checks it. The stub used to declare four parameters against
 * a three-parameter declaration and a three-argument call site; nothing
 * diagnosed it because this file did not include the header.
 *
 * Nothing in apps/iap calls it now: it is the file browser's entry
 * point and two of the three things it does wait for a human. Kept so
 * the guard fires if it comes back. */
bool ft_play_playlist(char *dir, char *file, char *sel)
{
    no_open_packet("ft_play_playlist"); (void)dir; (void)file; (void)sel;
  rbstub_calls.ft_play_playlist++; return true; }

/* The two calls that replaced it. playlist_create() reads a file, so it
 * is as blocking as the rest of the playlist API and gets the same
 * guard; rbstub_fail_playlist_create() stands in for a catalogue entry
 * that has gone. */
void rbstub_fail_playlist_create(bool fail)
{
    playlist_create_fails = fail;
}

int playlist_create(const char *dir, const char *file)
{
    no_open_packet("playlist_create"); (void)dir; (void)file;
    rbstub_calls.play_playlist++;

    /* The real one (apps/playlist.c:2074) returns -1 only when the
     * buffer allocation fails. A catalogue entry that has gone takes
     * the ordinary path and returns 0 with an empty queue --
     * add_indices_to_playlist()'s failure is discarded at :2095. The
     * stub used to model the missing file as -1, which is a failure
     * the real call cannot produce, and that hid a firmware bug: the
     * "== -1" test alone answered Success and started silence. */
    if (playlist_create_fails) {
        fake_playlist.amount = 0;
        fake_playlist.index = 0;
        fake_playlist.first_index = 0;
        return 0;
    }
    return 0;
}

int playlist_shuffle(int random_seed, int start_index)
{
    no_open_packet("playlist_shuffle"); (void)random_seed; (void)start_index;
    rbstub_calls.randomise++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Kernel                                                               */
/* ------------------------------------------------------------------ */

void queue_init(struct event_queue *q, bool register_queue)
{ (void)q; (void)register_queue; }
void queue_post(struct event_queue *q, long id, intptr_t data)
{
    (void)q;
    rbstub_calls.queue_post++;
    rbstub_calls.last_event = id;
    rbstub_calls.last_event_data = data;
}
void button_queue_post(long id, intptr_t data)
{ (void)data; rbstub_calls.button_post++; rbstub_calls.last_button_event = id; }
void queue_wait(struct event_queue *q, struct queue_event *ev)
{ (void)q; ev->id = 0; ev->data = 0; }

unsigned int create_thread(void (*fn)(void), void *stack, size_t stack_size,
                           unsigned flags, const char *name
                           IF_PRIO(, int priority) IF_COP(, unsigned core))
{ (void)fn; (void)stack; (void)stack_size; (void)flags; (void)name;
  return 1; /* non-zero: iap_setup() panics on 0 */ }

/* Record the interval the iAP layer last asked to be woken at. The
 * tick paces the button auto-release, so a test can tell a fast re-arm
 * from leaving a 1 Hz timer to run out. */
static struct timeout *saved_tmo;
static timeout_cb_type saved_cb;

void timeout_register(struct timeout *tmo, timeout_cb_type callback,
                      int ticks, intptr_t data)
{
    (void)data;
    saved_tmo = tmo;
    saved_cb = callback;
    rbstub_calls.timeout_ticks = ticks;
    rbstub_calls.timeouts++;
}

/* Run the registered callback and hand back the interval it asks for
 * next. iap_task() is static in iap-core.c, so this is the only way to
 * reach it -- and the interval it returns is what paces the button
 * auto-release and the notification cadence. */
int rbstub_run_timeout(void)
{
    if (!saved_cb)
        return -1;
    return saved_cb(saved_tmo);
}

unsigned sleep(unsigned ticks)
{
    no_open_packet("sleep");
    (void)ticks; return 0;
}
void yield(void)
{
    /* The primitive the guard's own comment names: yielding with a
     * packet half-built is what lets another thread rewind
     * iap_txnext under this one. */
    no_open_packet("yield");
}

/* Record subscriptions so a test can fire a playback event and observe
 * what the iAP layer transmits in response. iap_track_changed() is
 * static in iap-core.c, so this registration is the only way to reach it. */
#define RBSTUB_MAX_EVENTS 8
static struct {
    unsigned short id;
    void (*handler)(unsigned short, void *);
} fake_events[RBSTUB_MAX_EVENTS];
static int fake_event_n;

bool add_event(unsigned short id, void (*handler)(unsigned short, void *))
{
    for (int i = 0; i < fake_event_n; i++)
        if (fake_events[i].id == id && fake_events[i].handler == handler)
            return true;
    if (fake_event_n >= RBSTUB_MAX_EVENTS)
        return false;
    fake_events[fake_event_n].id = id;
    fake_events[fake_event_n].handler = handler;
    fake_event_n++;
    return true;
}

bool rbstub_fire_event(unsigned short id, void *data)
{
    bool fired = false;
    for (int i = 0; i < fake_event_n; i++)
        if (fake_events[i].id == id) {
            fake_events[i].handler(id, data);
            fired = true;
        }
    return fired;
}

void panicf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "\n*** PANIC: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, " ***\n");
    va_end(ap);
    rbstub_calls.panics++;
    /* A panic in firmware is fatal; make it fatal here too so a test
     * can never silently pass through one. */
    abort();
}

/* ------------------------------------------------------------------ */
/* Serial transport                                                     */
/* ------------------------------------------------------------------ */

void serial_bitrate(int rate) { (void)rate; }
int  tx_rdy(void)             { return 1; }
void tx_writec(unsigned char c) { (void)c; }

/* ------------------------------------------------------------------ */
/* Power / battery / misc                                              */
/* ------------------------------------------------------------------ */

void rbstub_set_battery(int pct)  { fake_battery = pct; }
int  battery_level(void)        { return fake_battery; }
unsigned int charger_input_state = 0;
int charge_state = 0;   /* DISCHARGING */

/* The charger IC's own line, one GPIO bit on each target
 * (power-6g.c:233, power-ipod.c:82). Distinct from charge_state, which
 * firmware/powermgmt.c:593 derives from charger_input_state alone --
 * and that derivation is why a test could set charge_state and
 * charger_input_state to a pair the hardware never produces. Drive
 * this instead. */
static bool fake_charging = false;
void rbstub_set_charging(bool on) { fake_charging = on; }
bool charging_state(void)         { return fake_charging; }
void reset_poweroff_timer(void) { }
void rbstub_set_hold(bool on)   { fake_hold = on; }
bool button_hold(void)          { return fake_hold; }
int  action_userabort(int tmo)  { (void)tmo; return 0; }
int  settings_save(void)
{
    no_open_packet("settings_save");
    rbstub_calls.settings_save++; return 0;
}
void usb_acknowledge(long id)   { (void)id; }

unsigned int mixer_get_frequency(void) { return fake_mixer_frequency; }
void rbstub_set_mixer_frequency(unsigned int f) { fake_mixer_frequency = f; }

/* The mixer rate the iAP layer may move to when an accessory cannot
 * take the current one. Recorded so a test can see it happen. */
void mixer_set_frequency(unsigned int samplerate)
{
    rbstub_calls.mixer_frequency = (int)samplerate;
    fake_mixer_frequency = samplerate;
}

/* sound_min(), sound_max() and sound_steps() are NOT stubbed: the real
 * firmware/sound.c is compiled into the harness so they read the actual
 * codec table generated from firmware/export/cs42l55.h. Stubbing them
 * made the codec-range case assert a constant against itself. */
bool audio_is_initialized = true;

/* ------------------------------------------------------------------ */
/* Time                                                                 */
/* ------------------------------------------------------------------ */

static struct tm fake_tm;
struct tm *get_time(void)
{
    no_open_packet("get_time");
    return &fake_tm;
}

/* Rockbox stores tm_year as years since 1900, the POSIX convention:
 * rtc-6g.c:47 and rtc_pcf50605.c:54 both do "buf[6] + 100", and
 * valid_time() (timefuncs.c:92) rejects anything outside 100..199.
 * Callers pass the A.D. year so a test reads naturally. */
void rbstub_set_date(int year_ad, int mon, int mday, int hour, int min)
{
    fake_tm.tm_year = year_ad - 1900;
    fake_tm.tm_mon  = mon - 1;
    fake_tm.tm_mday = mday;
    fake_tm.tm_hour = hour;
    fake_tm.tm_min  = min;
}

/* ------------------------------------------------------------------ */
/* LCD (only reached by the iAP debug screen, never by the tests)       */
/* ------------------------------------------------------------------ */

void lcd_clear_display(void) { }
void lcd_update(void)        { }
void lcd_setfont(int font)   { (void)font; }
void lcd_putsf(int x, int y, const unsigned char *fmt, ...) { (void)x; (void)y; (void)fmt; }

/* ------------------------------------------------------------------ */
/* Directory access (playlist catalog browsing in lingo 4)              */
/* ------------------------------------------------------------------ */

/* A catalogue the tests can populate. opendir() returned NULL
 * unconditionally, so nbr_total_playlists() was always 0 and every
 * SelectDBRecord for the Playlist category was refused before it
 * reached the code under test -- a case written against it read the
 * refusal as the behaviour it meant to check. */
void rbstub_set_playlist_catalog(const char *const *names, int n)
{
    catalog_n = (n > RBSTUB_MAX_CATALOG) ? RBSTUB_MAX_CATALOG : n;
    for (int i = 0; i < catalog_n; i++)
        catalog[i] = names[i];
}

DIR *opendir(const char *name)
{
    no_open_packet("opendir");
    (void)name;
    if (!catalog_n)
        return NULL;
    catalog_pos = 0;
    catalog_open = true;
    /* Not a real DIR, and nothing dereferences it -- readdir() and
     * closedir() below are the only users. */
    return (DIR *)&catalog_open;
}
struct dirent *readdir(DIR *d)
{
    no_open_packet("readdir");
    (void)d;
    if (!catalog_open || catalog_pos >= catalog_n)
        return NULL;
    memset(&catalog_ent, 0, sizeof(catalog_ent));
    snprintf((char *)catalog_ent.d_name, sizeof(catalog_ent.d_name),
             "%s", catalog[catalog_pos++]);
    return &catalog_ent;
}
int closedir(DIR *d)
{
    no_open_packet("closedir");
    (void)d; catalog_open = false; return 0;
}

/* ------------------------------------------------------------------ */
/* USB audio                                                            */
/* ------------------------------------------------------------------ */

/* Two flags, because the real ones are two independent statics set
 * fourteen lines apart in usb_audio_init_connection() (usb_audio.c).
 * One flag here made the three call sites -- iap-core.c's
 * audio_streaming gate and iap-lingo3.c's two volume refusals --
 * interchangeable, so swapping any of them was invisible. */
static bool fake_usb_audio_active = false;
static bool fake_usb_audio_streaming = false;
void rbstub_set_usb_audio_active(bool on) { fake_usb_audio_active = on; }
void rbstub_set_usb_audio_streaming(bool on) { fake_usb_audio_streaming = on; }

#ifdef HAVE_IAP_ACCESSORY_POLL
/* The real one lives in the target's UART driver and reads the
 * accessory-detect line over I2C, which is why iap_periodic() calls it
 * from the iAP thread rather than a tick. Counted here so a case can
 * see that the call is still made -- if it stops, a dock detach goes
 * unnoticed again and the next accessory inherits the last one's
 * session. The uart suite covers what the real one then does. */
int rbstub_accessory_polls;

void iap_accessory_poll(void) { rbstub_accessory_polls++; }
#endif

bool usb_audio_get_active(void)        { return fake_usb_audio_active; }
bool usb_audio_source_streaming(void)  { return fake_usb_audio_streaming; }
void usb_audio_set_source_sampling_frequency(unsigned int f) { (void)f; }

/* ------------------------------------------------------------------ */
/* String helpers Rockbox provides in firmware/common                   */
/* ------------------------------------------------------------------ */

#ifndef __APPLE__
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t n = (len >= size) ? size - 1 : len;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}
#endif

char *strmemccpy(char *dst, const char *src, size_t size)
{
    if (!size)
        return NULL;
    size_t len = strlen(src);
    if (len >= size) {
        memcpy(dst, src, size - 1);
        dst[size - 1] = '\0';
        return NULL;
    }
    memcpy(dst, src, len + 1);
    return dst + len;
}

/* ------------------------------------------------------------------ */
/* Codec driver                                                        */
/* ------------------------------------------------------------------ */

/* firmware/sound.c is compiled for real so sound_min()/sound_max() read
 * the codec table generated from firmware/export/cs42l55.h. It calls
 * down into the CS42L55 driver, which needs hardware; these absorb that.
 * Only the ranges matter to the iAP layer, and those come from the
 * header, not from these. */
/* The bottom of the volume path. Neither target defines
 * AUDIOHW_HAVE_CLIPPING, so firmware/sound.c's sound_set_volume() gets
 * here through the prescaler; either way this is the last stop before
 * the codec, and the only place that can tell whether a level was
 * actually applied or merely stored. */
void audiohw_set_volume(int l, int r)
{
    /* Production reaches this bypassing the guarded setvol():
     * iap-lingo3.c's mute calls sound_set_volume(sound_min(...))
     * directly, and on the target that ends in an i2c mutex. */
    no_open_packet("audiohw_set_volume");
    (void)r;
    rbstub_calls.sound_set_volume++;
    rbstub_calls.codec_volume = l;
}
void audiohw_set_lineout_volume(int l, int r) { (void)l; (void)r; }
void audiohw_set_channel(int v)         { (void)v; }
void audiohw_set_stereo_width(int v)    { (void)v; }
void audiohw_set_bass(int v)            { (void)v; }
void audiohw_set_bass_cutoff(int v)     { (void)v; }
void audiohw_set_treble(int v)          { (void)v; }
void audiohw_set_treble_cutoff(int v)   { (void)v; }
void audiohw_set_prescaler(int v)       { (void)v; }
void audiohw_set_pitch(int32_t v)       { (void)v; }
int32_t audiohw_get_pitch(void)         { return 0; }

/* ------------------------------------------------------------------ */
/* RF Tuner (ipodvideo only -- CONFIG_TUNER)                            */
/*                                                                      */
/* The tuner driver itself, firmware/drivers/tuner/ipod_remote_tuner.c, */
/* is compiled for real; only what it calls out to is stubbed. The RDS  */
/* pushes are recorded because that is the observable half of lingo 7   */
/* command 0x0C -- an accessory sends RDS payloads and the driver has   */
/* to hand the right bytes and length to the RDS layer.                 */
/* ------------------------------------------------------------------ */
#if CONFIG_TUNER

#include "rds.h"

struct rbstub_rds rbstub_rds;

/* Copied verbatim from firmware/tuner.c:32-41. It has to be, because
 * the driver reads freq_step and deemphasis out of it and turns them
 * into SetTunerMode bits: a table that disagrees with the real one
 * makes every region test reason about a device that does not exist.
 * This one did. Europe's step was 50000 where the real table says
 * 100000, Korea's was 100000/75 against 200000/50, and Italy and Other
 * were missing altogether -- zero-initialised, so a case that selected
 * either saw a channel spacing of 0 Hz. */
const struct fm_region_data fm_region_data[TUNER_NUM_REGIONS] = {
    [REGION_EUROPE]    = { 87500000, 108000000, 100000, 50 },
    [REGION_US_CANADA] = { 87900000, 107900000, 200000, 75 },
    [REGION_JAPAN]     = { 76000000,  90000000, 100000, 50 },
    [REGION_KOREA]     = { 87500000, 108000000, 200000, 50 },
    [REGION_ITALY]     = { 87500000, 108000000,  50000, 50 },
    [REGION_OTHER]     = { 87500000, 108000000,  50000, 50 }
};

void rds_init(void)  { rbstub_rds.inits++; }
void rds_reset(void) { rbstub_rds.resets++; }

void rds_push_info(enum rds_info_id info_id, uintptr_t data, size_t size)
{
    if (rbstub_rds.n >= RBSTUB_RDS_MAX)
        return;
    struct rbstub_rds_push *e = &rbstub_rds.push[rbstub_rds.n++];
    e->info_id = (int)info_id;
    e->size = size;
    /* Copy rather than alias: the driver's buffer is reused, and a test
     * comparing pointers would silently pass on stale bytes. Record the
     * requested size before clamping so an over-long push is visible. */
    size_t n = size > sizeof(e->data) ? sizeof(e->data) : size;
    memcpy(e->data, (const void *)data, n);
    e->copied = n;
}

unsigned short adc_read(int channel) { (void)channel; return 0; }

int find_first_set_bit(uint32_t val)
{
    if (val == 0)
        return 32;
    int n = 0;
    while (!(val & 1)) { val >>= 1; n++; }
    return n;
}

#endif /* CONFIG_TUNER */
