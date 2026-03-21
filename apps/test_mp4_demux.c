/* Quick test for mp4_demux - compile and run on host.
 * gcc -o test_mp4_demux test_mp4_demux.c mp4_demux.c -I../firmware/export -I../firmware/include
 *
 * Or just use this as a reference for the expected output. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

/* Minimal stubs for Rockbox types used by mp4_demux.c */
#define O_RDONLY 0  /* already defined, just in case */

#include "mp4_demux.h"

#define TEST_FILE "/Users/nux/Source/ipod-re/test/Getting Stronger, but so blue.m4v"

static uint32_t sample_sizes[65536];
static uint32_t chunk_offsets[8192];

int main(void)
{
    struct mp4v_demux_res res;
    int ret;

    printf("Opening: %s\n", TEST_FILE);

    ret = mp4v_demux_open(TEST_FILE, &res,
                          sample_sizes, 65536,
                          chunk_offsets, 8192,
                          NULL, 0, NULL, 0);

    if (ret < 0)
    {
        printf("FAILED to parse MP4\n");
        return 1;
    }

    printf("\n=== MP4 Video Track ===\n");
    printf("Format:      %c%c%c%c\n", SPLITFOURCC(res.format));
    printf("Resolution:  %ux%u\n", res.width, res.height);
    printf("Timescale:   %u\n", res.timescale);
    printf("AVC Profile: %u\n", res.avc_profile);
    printf("AVC Level:   %u\n", res.avc_level);
    printf("NALU len:    %u bytes\n", res.nalu_len_size);
    printf("Num SPS:     %u\n", res.num_sps);
    printf("Num PPS:     %u\n", res.num_pps);
    printf("Codec data:  %u bytes\n", res.codecdata_len);
    printf("\n");
    printf("Samples:     %u\n", res.num_samples);
    printf("STTS entries:%u\n", res.num_stts);
    printf("STSC entries:%u\n", res.num_stsc);
    printf("Chunks:      %u\n", res.num_stco);
    printf("Keyframes:   %u\n", res.num_stss);
    printf("mdat offset: 0x%x\n", res.mdat_offset);
    printf("mdat length: %u bytes\n", res.mdat_len);

    /* Print first few sample offsets */
    printf("\n=== First 10 samples ===\n");
    {
        uint32_t i;
        uint32_t limit = res.num_samples < 10 ? res.num_samples : 10;
        for (i = 0; i < limit; i++)
        {
            uint32_t offset, size;
            if (mp4v_get_sample_offset(&res, i, &offset, &size) == 0)
            {
                printf("  sample %3u: offset=0x%08x size=%5u %s\n",
                       i, offset, size,
                       mp4v_is_keyframe(&res, i) ? "[KEY]" : "");
            }
        }
    }

    /* Print first few keyframes */
    if (res.num_stss > 0)
    {
        uint32_t i;
        uint32_t limit = res.num_stss < 10 ? res.num_stss : 10;
        printf("\n=== First %u keyframes (of %u) ===\n", limit, res.num_stss);
        for (i = 0; i < limit; i++)
            printf("  keyframe at sample %u\n", res.stss[i]);
    }

    /* Print avcC hex dump */
    printf("\n=== avcC data (%u bytes) ===\n", res.codecdata_len);
    {
        uint32_t i;
        for (i = 0; i < res.codecdata_len && i < 64; i++)
        {
            printf("%02x ", res.codecdata[i]);
            if ((i + 1) % 16 == 0) printf("\n");
        }
        if (res.codecdata_len > 64)
            printf("... (%u more bytes)\n", res.codecdata_len - 64);
        printf("\n");
    }

    printf("SUCCESS\n");
    return 0;
}
