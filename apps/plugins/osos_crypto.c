/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * osos_crypto -- iPod Classic 6G (S5L8702) on-device IM3 crypto tool.
 *
 * READ-ONLY self-test/oracle for the CoverFlow persist-to-slot plan. It uses
 * the on-chip AES + SHA engines with the fused device keys (GID / UID), driven
 * exactly like firmware/target/arm/s5l8702/crypto-s5l8702.c (hwkeyaes/sha1),
 * to answer -- from hardware, on THIS device -- the questions that gate any
 * write to the osos slot:
 *
 *   1. Are the fused GID/UID AES keys usable at RUNTIME (from a plugin), or
 *      locked after the bootrom?  (If a GID-decrypt yields a valid ARM vector
 *      table, they work.)
 *   2. Is the osos body GID- or UID-encrypted?  (Which key decrypts to code.)
 *   3. Is our re-encrypt an EXACT inverse of decrypt (AES-CBC, IV=0)?  Proven
 *      by re-encrypting the decrypted body and comparing to the original
 *      ciphertext byte-for-byte -- the "self-consistency proof" that makes a
 *      future re-seal trustworthy rather than a gamble.
 *   4. Which key produces the header MAC info_sign@0x40?
 *
 * It writes ONLY to FAT32 files (a decrypted-body dump + a text report). It
 * NEVER writes to the firmware partition, NOR, or NAND. Nothing here can brick
 * the device -- worst case it produces a wrong report.
 *
 * Usage: "Open with" -> osos_crypto on an input file that is EITHER
 *   - a full osos IM3 image (starts with "8702"; header 0x800 + body), or
 *   - a raw encrypted osos body (e.g. genuine38_soso.bin) -- whole file.
 *
 * This program copyright (C) 2026, GPLv2. AES/SHA sequences derived from
 * crypto-s5l8702.c (C) 2009 Michael Sparmann.
 */

#include "plugin.h"

/* S5L8702 IM3 header (crypto-s5l8702.h struct Im3Info) */
#define IM3HDR_SZ     0x800
#define SIGN_SZ       16
#define SHA1_SZ       20
#define ENC_OFF       0x07   /* enc_type byte */
#define DATASZ_OFF    0x0c   /* body size, LE */
#define INFOSIGN_OFF  0x40   /* header MAC, 16 bytes */
#define INFOSIGN_LEN  0x40   /* im3_sign hashes hdr[0:0x40] */

/* clockgate bits (s5l87xx.h): CLOCKGATE_AES=10, CLOCKGATE_SHA=0, PWRCON(0) */
#define CG_AES  (1u << 10)
#define CG_SHA  (1u << 0)

/* ---- AES: exact replica of crypto-s5l8702.c hwkeyaes (whole-buffer CBC,
 *      IV=0), with clockgate inlined and rb->commit_discard_dcache().  ---- */
static void aes_run(int encrypt, uint32_t keyidx, void *data, uint32_t size)
{
    int i;
    PWRCON(0) &= ~CG_AES;                 /* clockgate_enable(AES, true) */
    for (i = 0; i < 4; i++) AESIV[i] = 0;
    AESUNKREG0 = 1;
    AESUNKREG0 = 0;
    AESCONTROL = 1;
    AESUNKREG1 = 0;
    AESTYPE = keyidx;
    AESTYPE2 = ~AESTYPE;
    AESUNKREG2 = 0;
    AESKEYLEN = encrypt ? 9 : 8;
    AESOUTSIZE = size;
    AESOUTADDR = data;
    AESINSIZE = size;
    AESINADDR = data;
    AESAUXSIZE = size;
    AESAUXADDR = data;
    AESSIZE3 = size;
    rb->commit_discard_dcache();
    AESGO = 1;
    while (!(AESSTATUS & 0xf));
    PWRCON(0) |= CG_AES;                  /* clockgate_enable(AES, false) */
}

/* ---- SHA-1: exact replica of crypto-s5l8702.c sha1 ---- */
static void sha1_run(void *data, uint32_t size, void *hash)
{
    int i, space;
    bool done = false;
    uint32_t tmp32[16];
    uint8_t *tmp8 = (uint8_t *)tmp32;
    uint32_t *databuf = (uint32_t *)data;
    uint32_t *hashbuf = (uint32_t *)hash;
    PWRCON(0) &= ~CG_SHA;                 /* clockgate_enable(SHA, true) */
    SHA1RESET = 1;
    while (SHA1CONFIG & 1);
    SHA1RESET = 0;
    SHA1CONFIG = 0;
    while (!done)
    {
        space = ((uint32_t)databuf) - ((uint32_t)data) - size + 64;
        if (space > 0)
        {
            for (i = 0; i < 16; i++) tmp32[i] = 0;
            if (space <= 64)
            {
                for (i = 0; i < 64 - space; i++)
                    tmp8[i] = ((uint8_t *)databuf)[i];
                tmp8[64 - space] = 0x80;
            }
            if (space >= 8)
            {
                tmp8[0x3b] = (size >> 29) & 0xff;
                tmp8[0x3c] = (size >> 21) & 0xff;
                tmp8[0x3d] = (size >> 13) & 0xff;
                tmp8[0x3e] = (size >> 5) & 0xff;
                tmp8[0x3f] = (size << 3) & 0xff;
                done = true;
            }
            for (i = 0; i < 16; i++) SHA1DATAIN[i] = tmp32[i];
        }
        else
            for (i = 0; i < 16; i++) SHA1DATAIN[i] = *databuf++;
        SHA1CONFIG |= 2;
        while (SHA1CONFIG & 1);
        SHA1CONFIG |= 8;
    }
    for (i = 0; i < 5; i++) *hashbuf++ = SHA1RESULT[i];
    PWRCON(0) |= CG_SHA;                  /* clockgate_enable(SHA, false) */
}

/* im3_sign: AES_keyidx( SHA1(data)[:16] ) -> sign[16] */
static void im3_sign_run(uint32_t keyidx, void *data, uint32_t size, void *sign)
{
    unsigned char hash[SHA1_SZ];
    sha1_run(data, size, hash);
    rb->memcpy(sign, hash, SIGN_SZ);
    aes_run(1 /*encrypt*/, keyidx, sign, SIGN_SZ);
}

static uint32_t rd32le(const unsigned char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

/* Count how many of the first 8 words look like an ARM vector table entry
 * (B/BL with any condition, or LDR pc,[pc,#imm]). A decrypted osos starts
 * with such a table; garbage (wrong key) will score ~0. */
static int arm_vectors_score(const unsigned char *p)
{
    int i, score = 0;
    for (i = 0; i < 8; i++)
    {
        uint32_t w = rd32le(p + i * 4);
        if ((w & 0x0e000000) == 0x0a000000)          /* xxxx101x = B/BL */
            score++;
        else if ((w & 0x0fff0000) == 0x059f0000)     /* ldr pc,[pc,#imm] */
            score++;
    }
    return score;
}

static void hexstr(const unsigned char *p, int n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    int i;
    for (i = 0; i < n; i++)
    {
        out[i * 2]     = hx[(p[i] >> 4) & 0xf];
        out[i * 2 + 1] = hx[p[i] & 0xf];
    }
    out[n * 2] = 0;
}

/* line-buffered report writer */
static int g_log = -1;
static void logf_line(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = rb->vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    line[n] = '\n';
    if (g_log >= 0) rb->write(g_log, line, n + 1);
    line[n] = 0;
    rb->splashf(0, "%s", line);   /* also show progress on screen briefly */
}

enum plugin_status plugin_start(const void *parameter)
{
    const char *path = parameter;
    int fd, i;
    off_t fsize;
    size_t bufsz;
    unsigned char *file;      /* full input (uncached alias) */
    unsigned char *work;      /* working copy of the body */
    unsigned char *body;      /* pointer into file at body start */
    uint32_t hdr_present, bodylen, aeslen, enc_type = 0;
    unsigned char h[64];
    char hx[64];

    if (!path) { rb->splash(HZ * 2, "No input file"); return PLUGIN_ERROR; }

    fd = rb->open(path, O_RDONLY);
    if (fd < 0) { rb->splash(HZ * 2, "Cannot open input"); return PLUGIN_ERROR; }
    fsize = rb->filesize(fd);
    if (fsize < IM3HDR_SZ + 0x40)
    { rb->close(fd); rb->splash(HZ * 2, "Input too small"); return PLUGIN_ERROR; }

    /* Two buffers: full file + a working copy of the body. Use the uncached
     * DRAM alias (S5L8702_UNCACHED_ADDR = +0x40000000) so AES DMA is coherent
     * without relying on cache maintenance across the whole span. */
    file = rb->plugin_get_audio_buffer(&bufsz);
    file = (unsigned char *)((uintptr_t)file + 0x40000000);
    if ((off_t)bufsz < fsize * 2 + 0x1000)
    { rb->close(fd); rb->splash(HZ * 3, "Buffer too small"); return PLUGIN_ERROR; }

    if (rb->read(fd, file, fsize) != fsize)
    { rb->close(fd); rb->splash(HZ * 2, "Read failed"); return PLUGIN_ERROR; }
    rb->close(fd);

    /* Detect a full IM3 image vs a raw encrypted body. */
    hdr_present = (rb->memcmp(file, "8702", 4) == 0);
    if (hdr_present)
    {
        enc_type = file[ENC_OFF];
        bodylen = rd32le(file + DATASZ_OFF);
        body = file + IM3HDR_SZ;
        if (bodylen == 0 || (off_t)(IM3HDR_SZ + bodylen) > fsize)
            bodylen = fsize - IM3HDR_SZ;   /* fall back to whatever's there */
    }
    else
    {
        body = file;
        bodylen = fsize;
    }
    aeslen = bodylen & ~0xfu;              /* AES-CBC over the 16-aligned body
                                            * (osos data_sz is 16- not 64-aligned,
                                            * e.g. 0xa4a570) -- same as hwkeyaes */

    g_log = rb->open("/osos_crypto.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);

    logf_line("osos_crypto: %s", path);
    logf_line("size=%ld hdr=%d enc_type=%lu bodylen=0x%lx aeslen=0x%lx",
              (long)fsize, (int)hdr_present, (unsigned long)enc_type,
              (unsigned long)bodylen, (unsigned long)aeslen);
    hexstr(body, 16, hx);
    logf_line("ciphertext[0:16]= %s", hx);

    /* --- Try GID then UID: which key decrypts the body to ARM code? --- */
    work = file + (((fsize + 0xfff) & ~0xfffu));   /* aligned scratch after file */
    int best_key = 0, best_score = -1;
    {
        int keys[2] = { 1 /*GID*/, 2 /*UID*/ };
        for (i = 0; i < 2; i++)
        {
            int sc;
            rb->memcpy(work, body, aeslen);
            aes_run(0 /*decrypt*/, keys[i], work, aeslen);
            sc = arm_vectors_score(work);
            hexstr(work, 16, hx);
            logf_line("decrypt key=%d (%s): vec_score=%d/8 plain[0:16]=%s",
                      keys[i], keys[i] == 1 ? "GID" : "UID", sc, hx);
            if (sc > best_score) { best_score = sc; best_key = keys[i]; }
        }

        if (best_score < 2)
        {
            logf_line("RESULT: no key produced ARM code (best=%d). Either the "
                      "fused keys are locked at runtime, or this is not a "
                      "%s-key osos.", best_score, "GID/UID");
            if (g_log >= 0) rb->close(g_log);
            rb->splash(HZ * 4, "Decrypt: no ARM code (see /osos_crypto.log)");
            return PLUGIN_OK;
        }

        logf_line("=> body decrypts with key=%d (%s), vec_score=%d/8",
                  best_key, best_key == 1 ? "GID" : "UID", best_score);

        /* Re-run decrypt with the winning key, keep plaintext in work. */
        rb->memcpy(work, body, aeslen);
        aes_run(0, best_key, work, aeslen);

        /* Dump the decrypted body for host-side patch-site + version check. */
        {
            int od = rb->open("/osos_plain.bin", O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (od >= 0)
            {
                rb->write(od, work, bodylen);
                rb->close(od);
                logf_line("wrote /osos_plain.bin (0x%lx bytes)",
                          (unsigned long)bodylen);
            }
        }

        /* --- Self-consistency: re-encrypt the plaintext, must reproduce the
         *     original ciphertext byte-for-byte (proves exact CBC/IV0/key). */
        aes_run(1 /*encrypt*/, best_key, work, aeslen);
        if (rb->memcmp(work, body, aeslen) == 0)
            logf_line("SELF-CONSISTENCY: re-encrypt == on-disk ciphertext "
                      "(0x%lx bytes) -- recipe EXACT.", (unsigned long)aeslen);
        else
        {
            uint32_t d = 0;
            for (i = 0; (uint32_t)i < aeslen; i++)
                if (work[i] != body[i]) { d = i; break; }
            logf_line("SELF-CONSISTENCY: MISMATCH at 0x%lx -- re-encrypt is NOT "
                      "the inverse; do not trust re-seal.", (unsigned long)d);
        }
    }

    /* --- Header MAC: which key reproduces info_sign@0x40? (IM3 images only) */
    if (hdr_present)
    {
        int keys[2] = { 1, 2 };
        hexstr(file + INFOSIGN_OFF, SIGN_SZ, hx);
        logf_line("on-disk info_sign@0x40 = %s", hx);
        for (i = 0; i < 2; i++)
        {
            im3_sign_run(keys[i], file, INFOSIGN_LEN, h);
            hexstr(h, SIGN_SZ, hx);
            logf_line("info_sign key=%d (%s) = %s %s", keys[i],
                      keys[i] == 1 ? "GID" : "UID", hx,
                      rb->memcmp(h, file + INFOSIGN_OFF, SIGN_SZ) == 0
                          ? "<== MATCH" : "");
        }
    }

    /* --- Optional RESEAL (writes a FAT32 file only; NEVER the slot) ---
     * The 6 CoverFlow patches live in the BODY, so the header (and its
     * info_sign@0x40) is unchanged; reseal is just: GID-encrypt the patched
     * plaintext body and splice it into the original image, keeping header +
     * cert verbatim. /osos_patched.img then differs from the on-disk osos ONLY
     * in the encrypted body. It boots iff the SecureROM does not RSA-verify the
     * body (see SECUREROM_DUMP.md go/no-go); persist_osos.py writes it to the
     * slot later, gated + backed up. Supply the patched body as
     * /osos_patch_body.bin (host: coverflow_patch.py on /osos_plain.bin). */
    if (hdr_present && best_key > 0 && bodylen == aeslen)
    {
        int pf = rb->open("/osos_patch_body.bin", O_RDONLY);
        if (pf >= 0)
        {
            off_t psz = rb->filesize(pf);
            if (psz != (off_t)bodylen)
            {
                logf_line("RESEAL skip: /osos_patch_body.bin %ld bytes, need "
                          "bodylen 0x%lx", (long)psz, (unsigned long)bodylen);
                rb->close(pf);
            }
            else
            {
                rb->read(pf, work, bodylen);
                rb->close(pf);
                if (arm_vectors_score(work) < 2)
                    logf_line("RESEAL WARN: patch body vec_score<2 (not ARM?) "
                              "-- writing anyway");
                aes_run(1 /*encrypt*/, best_key, work, aeslen);
                int of = rb->open("/osos_patched.img",
                                  O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (of >= 0)
                {
                    rb->write(of, file, IM3HDR_SZ);            /* header (as-is) */
                    rb->write(of, work, aeslen);              /* enc patched body */
                    if ((off_t)(IM3HDR_SZ + bodylen) < fsize) /* cert/footer tail */
                        rb->write(of, file + IM3HDR_SZ + bodylen,
                                  fsize - IM3HDR_SZ - bodylen);
                    rb->close(of);
                    logf_line("RESEAL: wrote /osos_patched.img (%ld bytes) = "
                              "header + GID-encrypted patched body + cert.",
                              (long)fsize);
                }
                else
                    logf_line("RESEAL: cannot open /osos_patched.img");
            }
        }
    }

    logf_line("DONE. Report: /osos_crypto.log  Plaintext: /osos_plain.bin");
    if (g_log >= 0) rb->close(g_log);
    rb->splash(HZ * 4, "osos_crypto done -> /osos_crypto.log");
    return PLUGIN_OK;
}
