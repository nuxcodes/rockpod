/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 * tuner for the ipod fm remote and other ipod remote tuners
 *
 * Copyright (C) 2009 Laurent Gautier
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "config.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "kernel.h"
#include "iap.h"
#include "tuner.h" /* tuner abstraction interface */
#include "adc.h"
#include "settings.h"
#include "power.h"
#include "rds.h"

static unsigned char tuner_param = 0x00, old_tuner_param = 0xFF;
/* temp var for tests to avoid looping execution in submenus settings*/
static int mono_mode = -1, old_region = -1;

int radio_present = 0;

static int tuner_frequency = 0;
static int tuner_signal_power = 0;
static bool radio_tuned = false;

static void rmt_tuner_signal_power(unsigned char value)
{
    tuner_signal_power = (int)(value);
}

void rmt_tuner_freq(unsigned int len, const unsigned char *buf)
{
    /* length currently unused */
    (void)len;

    /* Widen before shifting: buf[2] promotes to int, so a top byte of
     * 128 or more shifted left by 24 overflows a signed int, which is
     * undefined. An FM frequency never reaches that, but a malformed
     * packet from the accessory can. */
    unsigned int khz = ((unsigned int)buf[2] << 24) |
                       ((unsigned int)buf[3] << 16) |
                       ((unsigned int)buf[4] <<  8) |
                        (unsigned int)buf[5];

    /* MFi 4.7.24 (p.305), RetTunerFreq after a seek: "If no channel was
     * found, a tuner frequency value of 0xFFFFFFFF must be reported."
     *
     * That was taken for a frequency. 0xFFFFFFFF * 1000 wraps to
     * 4294966296 and lands in an int as -1000, which is below every
     * region's freq_min -- so a failed seek left the radio reporting a
     * negative frequency and radio_tuned set, and the UI showed a
     * station that was not there. */
    if (khz == 0xFFFFFFFFu)
    {
        radio_tuned = false;
        tuner_signal_power = 0;
        return;
    }

    tuner_frequency = khz *1000 ;
    radio_tuned = true;
    rmt_tuner_signal_power(buf[6]);
}

static void rmt_tuner_set_freq(int curr_freq)
{
    if (curr_freq != tuner_frequency)
    {
        radio_tuned = false;
        tuner_signal_power = 0;
        /* clear rds data */
        rds_reset();
        /* ex: 00 01 63 14 = 90.9MHz */
        unsigned char data[] = {0x07, 0x0B, 0x00, 0x01, 0x63, 0x14};

        if (curr_freq != 0)
        {
            unsigned int khz = curr_freq / 1000;
            data[2] = (khz >> 24) & 0xFF;
            data[3] = (khz >> 16) & 0xFF;
            data[4] = (khz >>  8) & 0xFF;
            data[5] = (khz >>  0) & 0xFF;
            iap_send_pkt(data, sizeof(data));
        }
    }
}

/* Tuner power, on its own.
 *
 * MFi 4.7.10 (p.295): "RF tuner state information, such as tuner
 * frequency, band, and so on, must be preserved by the accessory across
 * tuner on and off cycles" -- and "When the tuner power is turned off,
 * it must disable its audio output", which is what makes this the mute.
 * So powering the tuner is all a mute has to do, and re-programming it
 * afterwards is not merely wasteful. */
static void rmt_tuner_power(bool on)
{
    const unsigned char data[] = {0x07, 0x05, on ? 0x01 : 0x00};
    iap_send_pkt(data, sizeof(data));
}

/* The cold wake-up: invalidate this side's shadow of the accessory's
 * state and program it from scratch. Only RADIO_SLEEP wants this. */
/* Defined below; the wake-up sequence needs them to put the user's
 * region, channel spacing, deemphasis and force-mono back. */
static void rmt_tuner_region(int region);
static void rmt_tuner_set_param(unsigned char param);
static void set_deltafreq(int delta);
static void set_deemphasis(int deemphasis);
static void set_mono(int value);

static void rmt_tuner_sleep(int state)
{
    if (state == 0)
    {
        rds_init();
        tuner_param = 0x00;
        old_tuner_param = 0xFF;
        mono_mode = -1;
        old_region = -1;
        tuner_frequency = 0;
        radio_tuned = false;

        rmt_tuner_power(true);
        /* set rds on */
        const unsigned char data3[] = {0x07, 0x20, 0x40, 0x00, 0x00, 0x10 };
        iap_send_pkt(data3, sizeof(data3));
        /* boost gain.
         *
         * MFi Table 4-111 (p.289) marks 0x22-0x24 Reserved, so this is
         * not a documented command -- it comes from what Apple's own
         * firmware sends to this accessory. Kept because the Radio
         * Remote is the only accessory that reaches this path and it is
         * what it expects; changing it on the strength of the table
         * alone would be guessing at hardware. Recorded so the next
         * reader knows the table was checked. */
        const unsigned char data1[] = {0x07, 0x24, 0x06 };
        iap_send_pkt(data1, sizeof(data1));
        /* Tuner mode: the user's, not the accessory's default.
         *
         * This used to send SetTunerMode 0x00 outright. Table 4-135
         * (p.301) makes that byte 200 kHz resolution, stereo allowed
         * and 75 us deemphasis -- the US defaults. The shadows above
         * are invalidated in the same breath, so nothing re-sent the
         * real settings either.
         *
         * apps/radio/radio.c:222-228 re-applies RADIO_REGION and
         * RADIO_FORCE_MONO only inside "if (radio_status ==
         * FMRADIO_OFF)", so resuming from FMRADIO_PAUSED -- the
         * ordinary Play press on a paused radio -- skipped it. Every
         * region in firmware/tuner.c:33-38 except the US wants 50 us
         * and a 100 or 50 kHz grid, so after any pause and resume the
         * user heard the wrong deemphasis, the accessory's own seek
         * grid was twice as coarse as their region's and skipped half
         * the stations, and Force Mono was off with the setting still
         * showing on.
         *
         * Applying them here rather than widening radio.c's condition:
         * this is the accessory whose state was just invalidated, and
         * it is the only thing that knows that. */
        rmt_tuner_region(global_settings.fm_region);
        set_deltafreq(fm_region_data[global_settings.fm_region].freq_step
                      == 50000 ? 2 :
                      fm_region_data[global_settings.fm_region].freq_step
                      == 100000 ? 1 : 0);
        set_deemphasis(fm_region_data[global_settings.fm_region].deemphasis
                       == 50 ? 1 : 0);
        set_mono(global_settings.fm_force_mono ? 1 : 0);
        rmt_tuner_set_param(tuner_param);
        /* set volume */
        unsigned char data2[] = {0x03, 0x09, 0x04, 0x00, 0x00 };
        /* Was (volume + 58) * 4, a scale that fits no codec this
         * firmware runs on: at the WM8758's +6 dB maximum it produced 0,
         * telling the accessory to play at silence. This is the same
         * lingo 3 / event 0x04 packet iap_periodic() builds, so it has
         * to use the same conversion or the two disagree on the wire
         * inside one session. */
        /* The UI scale, not the absolute one. Table 4-61 (p.258) gives
         * event 0x04 "Byte 1: UI Volume Level", and p.261 says the UI
         * level is "normalized to volume limit settings" where the
         * absolute one is not. 1050bb2f94 fixed the scale here and
         * reached for the wrong one of the pair -- its own comment says
         * this has to match what iap_periodic() sends, and that uses
         * iap_volume_to_ui_byte(). With a volume limit in force the two
         * disagree: at the limit this said 128 where the event requires
         * 255. */
        data2[4] = iap_volume_to_ui_byte(global_status.volume);
        /* Sent whether or not the accessory enabled event 0x04, where
         * iap_periodic() gates the identical packet on
         * device.notifications. That is a deliberate difference, not an
         * oversight: this is the Radio Remote's wake-up sequence, it is
         * the only accessory that reaches this path, and it has a
         * volume readout to populate before it has had a chance to
         * subscribe to anything. An accessory that did not ask for the
         * event can ignore a notification it did not enable; one whose
         * display starts blank cannot recover until the volume next
         * moves. Reviewed against MFi 4.3.12 (p.257) and kept. */
        iap_send_pkt(data2, sizeof(data2));
    }
    else
    {
        /* unbooste gain */
        const unsigned char data[] = {0x07, 0x24, 0x00};
        iap_send_pkt(data, sizeof(data));
        /* set rds off */
        const unsigned char data1[] = {0x07, 0x20, 0x00, 0x00, 0x00, 0x00 };
        iap_send_pkt(data1, sizeof(data1));
        rmt_tuner_power(false);
    }
}

void rmt_tuner_scan(int param)
{
    const unsigned char data[] = {0x07, 0x11, 0x08};  /* RSSI level */
    unsigned char updown = 0x00;
    radio_tuned = false;
    iap_send_pkt(data, sizeof(data));

    if (param == 1)
    {
        updown = 0x07;  /* scan up */
    }
    else if (param == -1)
    {
        updown = 0x08;  /* scan down */
    }
    else if (param == 10)
    {
        updown = 0x01;  /* scan up starting from beginning of the band */
    }
    unsigned char data1[] = {0x07, 0x12, updown};
    iap_send_pkt(data1, sizeof(data1));
}

static void rmt_tuner_mute(int value)
{
    /* mute flag off (play) */
    /* The Apple Tuner does NOT appear to support muting. The Apple
     * firmware turns the power off when pressing pause on the iPod
     * or on the Tuner Remote.
     */
    if (value)
    {
        /* mute flag on (pause) */
        unsigned char data[] = {0x03, 0x09, 0x03, 0x02};
        iap_send_pkt(data, sizeof(data));
        rmt_tuner_power(false);
    }
    else
    {
        unsigned char data[] = {0x03, 0x09, 0x03, 0x01};
        iap_send_pkt(data, sizeof(data));
        rmt_tuner_power(true);
    }
}

static void rmt_tuner_region(int region)
{
    if (region != old_region)
    {
        const struct fm_region_data *rd = &fm_region_data[region];
        unsigned char data[] = {0x07, 0x08, 0x00};
                /* Apple MFi Accessory Firmware Spec R46 now lists
                 * the following bands
                 * ID00 AM 520-1710Khz      Not Supported
                 * ID02 Japan 76-90Mkz 100Khz 50/75uS
                 * ID01 87.5-108Mhz US 200Khz 75uS, EU 100Kz 50uS
                 * ID03 76-108Mhz Wideband. Not Supported
         */
        if (rd->freq_min == 76000000)
        {
            data[2] = 0x02; /* japan band */
        }
        else
        {
            data[2] = 0x01; /* us/europe band */
        }
        iap_send_pkt(data, sizeof(data));
        sleep(HZ/100);
        old_region = region;
    }
}

/* set stereo/mono, deemphasis, delta freq... */
static void rmt_tuner_set_param(unsigned char tuner_param)
{
    if(tuner_param != old_tuner_param)
    {
        unsigned char data[] = {0x07, 0x0E, 0x00};

        data[2] = tuner_param;
        iap_send_pkt(data, sizeof(data));
        old_tuner_param = tuner_param;
    }
}

static void set_deltafreq(int delta)
{
    tuner_param &= 0xFC;
    switch (delta)
    {
        case 1:
        {
            /* 100KHz */
            tuner_param |= 0x01;
            break;
        }
        case 2:
        {
            /* 50KHz */
            tuner_param |= 0x02;
        break;
        }

        default:
        {
            /* 200KHz */
            tuner_param |= 0x00;
            break;
        }
    }
}

static void set_deemphasis(int deemphasis)
{
    tuner_param &= 0xBF;
    switch (deemphasis)
    {
        case 1:
        {
            tuner_param |= 0x40;
            /* 50uS */
            break;
        }
        default:
        {
            tuner_param |= 0x00;
            /* 75uS */
            break;
        }
    }
}

static void set_mono(int value)
{
    /* The clear that used to be here, outside the guard, dropped bit 4
     * from the cached parameter byte on every call -- including the
     * calls that change nothing and send nothing. tuner_param then
     * disagreed with what the tuner actually holds, and the next write
     * from any other setting carried force-mono off with it.
     *
     * apps/radio/radio.c sets the region and then force-mono on every
     * entry to the radio screen, so the third entry sent the parameter
     * byte with bit 4 clear while the user's setting still said force
     * mono. The clear inside the guard, below, is the one that belongs
     * here: it makes room for the bit that is about to be written. */
    if (value != mono_mode)
    {
        tuner_param &= 0xEF;
        if (value == 1)
            tuner_param |= 0x10;
        rmt_tuner_set_param(tuner_param);
        sleep(HZ/100);
        mono_mode = value;
    }
}

static bool reply_timeout(void)
{
    int timeout = 0;

    sleep(HZ/50);
    do
    {
        sleep(HZ/50);
        timeout++;
    }
    while((ipod_rmt_tuner_get(RADIO_TUNED) == 0) && (timeout < TIMEOUT_VALUE));

    return (timeout >= TIMEOUT_VALUE);
}

void rmt_tuner_rds_data(unsigned int len, const unsigned char *buf)
{
    /* The caller checks for four bytes, which is what buf[2] and the
     * buf+4 payload start need. How much payload there actually is
     * still varies with the packet: RetRdsData is "0xNN" bytes in
     * Table 4-111 (p.290), so nothing fixes it. */
    if (len < 4)
        return;

    unsigned int avail = len - 4;

    if (buf[2] == 0x1E)
    {
        /* The station name is eight characters, but only as many as
         * the packet carries. rds_push_info() clamps its size against
         * the destination, never the source, so asking for eight from
         * a five-byte packet read three bytes past it. */
        rds_push_info(RDS_INFO_PS, (uintptr_t)(buf+4), MIN(avail, 8u));
    }
    else if(buf[2] == 0x04)
    {
        rds_push_info(RDS_INFO_RT, (uintptr_t)(buf+4), avail);
    }
}

/* tuner abstraction layer: set something to the tuner */
int ipod_rmt_tuner_set(int setting, int value)
{
    switch(setting)
    {
        case RADIO_SLEEP:
        {
            rmt_tuner_sleep(value);
            sleep(HZ/10);
            if(value)
            {
                tuner_frequency = 0;
            }
            break;
        }

        case RADIO_FREQUENCY:
        {
            rmt_tuner_set_freq(value);
            if (reply_timeout())
                return 0;
            break;
        }

        case RADIO_SCAN_FREQUENCY:
        {
            const struct fm_region_data * const fmr =
            &fm_region_data[global_settings.fm_region];

            /* case: scan for presets, back to beginning of the band */
            if (radio_tuned && (value == fmr->freq_min))
            {
                tuner_set(RADIO_FREQUENCY,value);
            }

            /* scan through frequencies */
            if (radio_tuned)
            {
                /* Out of band, either end. This was && , which needs
                 * a frequency at or below the minimum and at or above
                 * the maximum at the same time -- unsatisfiable for
                 * every entry in fm_region_data, so the re-tune never
                 * ran and a scan that wandered outside the band stayed
                 * there. */
                if ((tuner_frequency <= fmr->freq_min)
                    || (tuner_frequency >= fmr->freq_max))
                {
                    tuner_set(RADIO_FREQUENCY,value);
                }
                /* scan down */
                if(value < tuner_frequency)
                    rmt_tuner_scan(-1);
                /* scan up */
                else
                    rmt_tuner_scan(1);

                sleep(HZ/10);
                if (reply_timeout())
                {
                    tuner_set(RADIO_FREQUENCY,value);
                    rmt_tuner_scan(1);
                    if (reply_timeout() == true)
                        return 0;
                }
                radio_tuned = false;
            }

            if (tuner_frequency == value)
            {
                radio_tuned = true;
                return 1;
            }
            else
            {
                radio_tuned = false;
                return 0;
            }
        }

        case RADIO_MUTE:
        {
            /* mute flag sent to accessory */
            rmt_tuner_mute(value);
            break;
        }

        case RADIO_REGION:
        {
            /* The latest MFi Accessory Firmware Document I have lists the
             * following regions
             * US 87.5-108Mhz 200Khz 75uS
             * US/EU 87.5-108Mhz 100Khz 75/50uS
             * JP 76.0-90Mhz 100Mhz 50/75uS
             *
             * with the following bands
             * 0x00 AM WordlWide 520-1710Khz
             * 0x01 FM EU 87.5-108.0Mhz
             * 0x02 FM JP 76.0-90.0Mhz
             * 0x03 FM Wide 76.0-108.0Mhz
             *
             *
             * A 7G Classic with the latest Apple Firmware returns the following
                         * regions with the settings listed
             * Americas   87.5-108 200Khz 75uS
             * Asia       87.5-108 100Khz 75uS
             * Australia  87.5-108 200Khz 75uS
             * Europe     87.5-108 100Khz 75uS
             * Japan      76.0-90. 100Kz  75uS
             */
            const struct fm_region_data *rd = &fm_region_data[value];
            int band = (rd->freq_min == 76000000) ? 2 : 0;
            int spacing = (100000 / rd->freq_step);
            int deemphasis = (rd->deemphasis == 50) ? 1 : 0;

            rmt_tuner_region(band);
            set_deltafreq(spacing);
            set_deemphasis(deemphasis);
            rmt_tuner_set_param(tuner_param);
            break;
        }

        case RADIO_FORCE_MONO:
        {
            set_mono(value);
            break;
        }

        default:
            return -1;
    }
    return 1;
}

/* tuner abstraction layer: read something from the tuner */
int ipod_rmt_tuner_get(int setting)
{
    int val = -1; /* default for unsupported query */

    switch(setting)
    {
        case RADIO_PRESENT:
            val = radio_present;
            if (val)
            {
                /* if accessory disconnected */
                if(adc_read(ADC_ACCESSORY) >= 10)
                {
                    radio_present = 0;
                    val = 0;
                }
            }
            break;

        /* radio tuned: yes no */
        case RADIO_TUNED:
            val = 0;
            if (radio_tuned)
                val = 1;
            break;

        /* radio is always stereo */
        /* we can't know when it's in mono mode, depending of signal quality */
        /* except if it is forced in mono mode */
        case RADIO_STEREO:
            val = true;
            break;
    }
    return val;
}
