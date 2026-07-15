/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2005 by Dave Chapman
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
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "config.h"

#include "inttypes.h"
#include "cpu.h"
#include "system.h"
#include "lcd.h"
#include "../kernel-internal.h"
#include "file_internal.h"
#include "storage.h"
#include "disk.h"
#include "font.h"
#include "backlight.h"
#include "backlight-target.h"
#include "button.h"
#include "panic.h"
#include "power.h"
#include "file.h"
#include "common.h"
#include "rb-loader.h"
#include "loader_strerror.h"
#include "version.h"
#include "powermgmt.h"
#include "usb.h"
#ifdef HAVE_SERIAL
#include "serial.h"
#endif

#include "s5l87xx.h"
#include "clocking-s5l8702.h"
#include "spi-s5l8702.h"
#include "i2c-s5l8702.h"
#include "gpio-s5l8702.h"
#include "pmu-target.h"
#include "dma-s5l8702.h"
#if defined(IPOD_6G) || defined(IPOD_NANO3G)
#include "norboot-target.h"
#endif


#define ERR_RB      0
#define ERR_OF      1
#define ERR_STORAGE 2
#define ERR_LBA28   3

/* Safety measure - maximum allowed firmware image size.
   The largest known current (October 2009) firmware is about 6.2MB so
   we set this to 8MB.
*/
#define MAX_LOADSIZE (8*1024*1024)

#define LCD_RBYELLOW    LCD_RGBPACK(255,192,0)
#define LCD_REDORANGE   LCD_RGBPACK(255,70,0)
#define LCD_GREEN       LCD_RGBPACK(0,255,0)

extern void bss_init(void);
extern uint32_t _movestart;
extern uint32_t start_loc;

extern int line;

#ifndef S5L87XX_DEVELOPMENT_BOOTLOADER
#ifdef HAVE_BOOTLOADER_USB_MODE
static void usb_mode(void)
{
    int button;

    verbose = true;

    printf("Entering USB mode...");

    powermgmt_init();

    /* The code will ask for the maximum possible value */
    usb_charging_enable(USB_CHARGING_ENABLE);

    usb_init();
    usb_start_monitoring();

    /* Wait until USB is plugged */
    while (usb_detect() != USB_INSERTED)
    {
        printf("Plug USB cable");
        line--;
        sleep(HZ/10);
    }

    while(1)
    {
        button = button_get_w_tmo(HZ/10);

        if (button == SYS_USB_CONNECTED)
            break; /* Hit */

        if (usb_detect() == USB_EXTRACTED)
            break; /* Cable pulled */

        /* Wait for threads to connect or cable is pulled */
        printf("USB: Connecting...");
        line--;
    }

    if (button == SYS_USB_CONNECTED)
    {
        /* Got the message - wait for disconnect */
        printf("Bootloader USB mode");

        /* Ack the SYS_USB_CONNECTED polled from the button queue */
        usb_acknowledge(SYS_USB_CONNECTED_ACK, button_get_data());

        while(1)
        {
            button = button_get_w_tmo(HZ/2);
            if (button == SYS_USB_DISCONNECTED)
                break;
        }
    }

    /* We don't want the HDD to spin up if the USB is attached again */
    usb_close();
    printf("USB mode exit     ");
}
#endif /* HAVE_BOOTLOADER_USB_MODE */

void fatal_error(int err)
{
    verbose = true;

    /* System font is 6 pixels wide */
    line++;
    switch (err)
    {
        case ERR_RB:
#ifdef HAVE_BOOTLOADER_USB_MODE
            usb_mode();
            printf("Hold MENU+SELECT to reboot");
            break;
#endif
        case ERR_STORAGE:
            printf("Hold MENU+SELECT to reboot");
            printf("then SELECT+PLAY for disk mode");
            break;
        case ERR_OF:
            printf("Hold MENU+SELECT to reboot");
            printf("and enter Rockbox firmware");
            break;
        case ERR_LBA28:
            printf("Hold MENU+SELECT to reboot");
            printf("and LEFT if you are REALLY sure");
            break;
    }

#if (CONFIG_STORAGE & STORAGE_ATA)
    if (ide_powered())
        ata_sleepnow(); /* Immediately spindown the disk. */
#endif

    line++;
    lcd_set_foreground(LCD_REDORANGE);
    while (1) {
        lcd_puts(0, line, button_hold() ? "Hold switch on!"
                                        : "               ");
        lcd_update();
    }
}

#if (CONFIG_STORAGE & STORAGE_ATA)
extern unsigned short battery_level_disksafe;
static void battery_trap(void)
{
    int vbat, old_verb;
    int th = 50;

    old_verb = verbose;
    verbose = true;

    usb_charging_maxcurrent_change(100);

    while (1)
    {
        vbat = _battery_voltage();

        /*  Two reasons to use this threshold (may require adjustments):
         *  - when USB (or wall adaptor) is plugged/unplugged, Vbat readings
         *    differ as much as more than 200 mV when charge current is at
         *    maximum (~340 mA).
         *  - RB uses some sort of average/compensation for battery voltage
         *    measurements, battery icon blinks at battery_level_disksafe,
         *    when the HDD is used heavily (large database) the level drops
         *    to battery_level_shutoff quickly.
         */
        if (vbat >= battery_level_disksafe + th)
            break;
        th = 200;

        if (power_input_status() != POWER_INPUT_NONE) {
            lcd_set_foreground(LCD_RBYELLOW);
            printf("Low battery: %d mV, charging...     ", vbat);
            sleep(HZ*3);
        }
        else {
            /* Wait for the user to insert a charger */
            int tmo = 10;
            lcd_set_foreground(LCD_REDORANGE);
            while (1) {
                vbat = _battery_voltage();
                printf("Low battery: %d mV, power off in %d ", vbat, tmo);
                if (!tmo--) {
                    /* Raise Vsysok (hyst=0.02*Vsysok) to avoid PMU
                       standby<->active looping */
                    if (vbat < 3200)
                        pmu_write(PCF5063X_REG_SVMCTL, 0xA /*3200mV*/);
                    power_off();
                }
                sleep(HZ*1);
                if (power_input_status() != POWER_INPUT_NONE)
                    break;
                line--;
            }
        }
        line--;
    }

    verbose = old_verb;
    lcd_set_foreground(LCD_WHITE);
    printf("Battery status ok: %d mV            ", vbat);
}
#endif /* CONFIG_STORAGE & STORAGE_ATA */
#endif /* S5L87XX_DEVELOPMENT_BOOTLOADER */

static int launch_onb(int clkdiv)
{
#if defined(IPOD_6G) || defined(IPOD_NANO3G)
    /* SPI clock = PClk/(clkdiv+1) */
    spi_clkdiv(SPI_PORT, clkdiv);

    /* Actually IRAM1_ORIG contains current RB bootloader IM3 header,
       it will be replaced by ONB IM3 header, so this function must
       be called once!!! */
    struct Im3Info *hinfo = (struct Im3Info*)IRAM1_ORIG;

    /* Loads ONB in IRAM0, exception vector table is destroyed !!! */
    int rc = im3_read(
            NORBOOT_OFF + im3_nor_sz(hinfo), hinfo, (void*)IRAM0_ORIG);

    if (rc != 0) {
        /* Restore exception vector table */
        memcpy((void*)IRAM0_ORIG, &_movestart, 4*(&start_loc-&_movestart));
        commit_discard_idcache();
        return rc;
    }

    /* Disable all external interrupts */
    eint_init();

    commit_discard_idcache();

    /* Branch to start of IRAM */
    asm volatile("mov pc, %0"::"r"(IRAM0_ORIG));
    while(1);
#elif defined(IPOD_NANO4G)
    (void) clkdiv;

    lcd_set_foreground(LCD_REDORANGE);
    printf("Not implemented");

    line++;
    lcd_set_foreground(LCD_RBYELLOW);
    printf("Press SELECT to continue");
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);

    return 0;
#endif
}

/* Launch OF when kernel mode is running */
static int kernel_launch_onb(void)
{
    disable_irq();
    int rc = launch_onb(3); /* 54/4 = 13.5 MHz. */
    enable_irq();
    return rc;
}

#if defined(IPOD_6G) || defined(IPOD_NANO3G)
static inline uint32_t onb_get_uint32le(unsigned char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

/* Dump ONB (Original NOR Boot) to files on the FAT32 partition.
 * Requires storage + filesystem to be initialized and disk mounted.
 * Writes:
 *   /onb_raw.bin       - raw IM3 image (header + encrypted body)
 *   /onb_decrypted.bin - decrypted ARM code (loads at IRAM0_ORIG)
 *   /onb_header.bin    - IM3 header only (0x800 bytes)
 * Returns 0 on success, negative on error.
 */
static int dump_onb_to_disk(void)
{
    /* Read primary bootloader (RB) IM3 header to find ONB offset */
    struct Im3Info hinfo;
    int rc = im3_read(NORBOOT_OFF, &hinfo, NULL);
    if (rc != 0) {
        printf("Primary BL read error: %d", rc);
        return -1;
    }

    unsigned bl_sz = im3_nor_sz(&hinfo);
    unsigned onb_offset = NORBOOT_OFF + bl_sz;
    printf("ONB at NOR offset 0x%x", onb_offset);

    /* Read ONB IM3 header (just header, no body yet) */
    rc = im3_read(onb_offset, &hinfo, NULL);
    if (rc != 0) {
        printf("ONB header read error: %d", rc);
        return -2;
    }

    uint32_t onb_data_sz = onb_get_uint32le(hinfo.data_sz);
    uint32_t onb_entry = onb_get_uint32le(hinfo.entry);
    unsigned onb_total_raw = IM3HDR_SZ + onb_data_sz;
    printf("ONB: data_sz=0x%x entry=0x%x enc=%d",
            onb_data_sz, onb_entry, hinfo.enc_type);
    lcd_update();

    /* Use DRAM as buffer (well above bootloader/firmware usage) */
    unsigned char *raw_buf = (unsigned char *)(DRAM_ORIG + 0x2000000);
    unsigned char *dec_buf = (unsigned char *)(DRAM_ORIG + 0x2100000);

    /* 1. Dump raw ONB (header + encrypted body) from NOR */
    printf("Reading raw ONB from NOR...");
    lcd_update();
    bootflash_init(SPI_PORT);
    bootflash_read(SPI_PORT, onb_offset, onb_total_raw, raw_buf);
    bootflash_close(SPI_PORT);

    int fd = open("/onb_raw.bin", O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if (fd < 0) {
        printf("Failed to create /onb_raw.bin");
        return -3;
    }
    write(fd, raw_buf, onb_total_raw);
    close(fd);
    printf("Wrote /onb_raw.bin (%u bytes)", onb_total_raw);

    /* 2. Dump decrypted ONB body via im3_read */
    printf("Decrypting ONB...");
    lcd_update();
    rc = im3_read(onb_offset, &hinfo, dec_buf);
    if (rc != 0) {
        printf("ONB decrypt error: %d", rc);
        return -4;
    }

    fd = open("/onb_decrypted.bin", O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if (fd < 0) {
        printf("Failed to create /onb_decrypted.bin");
        return -5;
    }
    write(fd, dec_buf, onb_data_sz);
    close(fd);
    printf("Wrote /onb_decrypted.bin (%u bytes)", onb_data_sz);

    /* 3. Dump the IM3 header separately for analysis */
    fd = open("/onb_header.bin", O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if (fd < 0) {
        printf("Failed to create /onb_header.bin");
        return -6;
    }
    write(fd, raw_buf, IM3HDR_SZ);
    close(fd);
    printf("Wrote /onb_header.bin (%u bytes)", IM3HDR_SZ);

    printf("ONB dump complete!");
    return 0;
}
#endif /* IPOD_6G || IPOD_NANO3G */

/*  The boot sequence is executed on power-on or reset. After power-up
 *  the device could come from a state of hibernation, OF hibernates
 *  the iPod after an inactive period of ~30 minutes, on this state the
 *  SDRAM is in self-refresh mode.
 *
 *  t0 = 0
 *     S5L8702 BOOTROM loads an IM3 image located at NOR:
 *     - IM3 header (first 0x800 bytes) is loaded at IRAM1_ORIG
 *     - IM3 body (decrypted RB bootloader) is loaded at IRAM0_ORIG
 *     The time needed to load the RB bootloader (~100 Kb) is estimated
 *     on 200~250 ms. Once executed, RB booloader moves itself from
 *     IRAM0_ORIG to IRAM1_ORIG+0x800, preserving current IM3 header
 *     that contains the NOR offset where the ONB (original NOR boot),
 *     is located (see dualboot.c for details).
 *
 *  t1 = ~250 ms.
 *     If the PMU is hibernated, decrypted ONB (size 128Kb) is loaded
 *       and executed, it takes ~120 ms. Then the ONB restores the
 *       iPod to the state prior to hibernation.
 *     If not, initialize system and RB kernel, wait for t2.
 *
 *  t2 = ~650 ms.
 *     Check user button selection.
 *     If OF, diagmode, or diskmode is selected then launch ONB.
 *     If not, wait for LCD initialization.
 *
 *  t3 = ~700,~900 ms. (lcd_type_01,lcd_type_23)
 *     LCD is initialized, baclight ON.
 *     Wait for HDD spin-up.
 *
 *  t4 = ~2600,~2800 ms.
 *     HDD is ready.
 *     If hold switch is locked, then load and launch ONB.
 *     If not, load rockbox.ipod file from HDD.
 *
 *  t5 = ~2800,~3000 ms.
 *     rockbox.ipod is executed.
 */

#ifdef S5L87XX_DEVELOPMENT_BOOTLOADER
#include "piezo.h"
#include "lcd-s5l8702.h"
extern int lcd_type;

static uint16_t alive[] = { 500,100,0, 0 };
static uint16_t alivelcd[] = { 2000,200,0, 0 };

#ifdef HAVE_LCD_SLEEP
static void sleep_test(void)
{
    int sleep_tmo = 5;
    int awake_tmo = 3;

    lcd_clear_display();
    lcd_set_foreground(LCD_WHITE);
    line = 0;

    printf("Entering LCD sleep mode in %d seconds,", sleep_tmo);
    printf("during sleep mode you will see a white");
    printf("screen for about %d seconds.", awake_tmo);
    while (sleep_tmo--) {
        printf("Sleep in %d...", sleep_tmo);
        sleep(HZ*1);
    }
    lcd_sleep();
    sleep(HZ*awake_tmo);
    lcd_awake();

    line++;
    printf("Awake!");

    line++;
    lcd_set_foreground(LCD_RBYELLOW);
    printf("Press SELECT to continue");
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);
}
#endif

static void pmu_info(void)
{
    int loop = 0;

    lcd_clear_display();
    lcd_update();
    while (button_status() != BUTTON_NONE);

    while (1)
    {
        lcd_set_foreground(LCD_WHITE);
        lcd_clear_display();
        line = 0;
        printf("loop: %d", loop++);

        for (int i = 0; i < 128; i += 8)
        {
            unsigned char buf[8];

#if defined(IPOD_NANO3G)
            if (i == 0) {
                static int flip = 0;
                if (flip) {
                    pmu_write(6, 0xff);
                    pmu_write(7, 0xff);
                }
                else {
                    pmu_write(6, 0xe7);
                    pmu_write(7, 0xfe);
                }
                flip ^= 1;
            }
#elif defined(IPOD_NANO4G)
            if (i == 120)
                for (int j = 0; j < 8; j++)
                    pmu_write(i+j, j);
#endif
            for (int j = 0; j < 8; j++)
                buf[j] = pmu_read(i+j);

            printf(" %2x: %2x %2x %2x %2x %2x %2x %2x %2x", i,
                    buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);
        }
        line++;
        printf("USB: %s    ", (usb_detect() == USB_INSERTED) ? "inserted" : "not inserted");
#if CONFIG_CHARGING
        printf("Firewire: %s    ", pmu_firewire_present() ? "inserted" : "not inserted");
#endif
#ifdef IPOD_ACCESSORY_PROTOCOL
        printf("Accessory: %s    ", pmu_accessory_present() ? "inserted" : "not inserted");
#endif
        printf("Hold Switch: %s  ", pmu_holdswitch_locked() ? "locked" : "unlocked");
        line++;
        lcd_set_foreground(LCD_RBYELLOW);
        printf("Press SELECT to continue");
        if (button_status() == BUTTON_SELECT)
            break;
        sleep(HZ/2);
    }
}

static void gpio_info(void)
{
    int loop = 0;

    lcd_clear_display();

    while (1)
    {
        lcd_set_foreground(LCD_WHITE);
        lcd_clear_display();
        line = 0;
        printf("loop: %d", loop++);
        for (int i = 0; i < GPIO_N_GROUPS; i ++)
        {
            printf(" %x: %8x %2x %4x %2x %2x", i,
                    PCON(i), PDAT(i), PUNA(i), PUNB(i), PUNC(i));
        }
        line++;
        lcd_set_foreground(LCD_RBYELLOW);
        printf("Press SELECT to continue");
        if (button_status() == BUTTON_SELECT)
            break;
        sleep(HZ/5);
    }
}

static void run_of(void)
{
    int tmo = 5;
    lcd_clear_display();
    lcd_set_foreground(LCD_WHITE);
    line = 0;
    while (tmo--) {
        printf("Booting OF in %d...", tmo);
        sleep(HZ*1);
    }

    int rc = kernel_launch_onb();
    printf("Load OF error: %d", rc);
    sleep(HZ*10);
}

#if defined(IPOD_6G) || defined(IPOD_NANO3G)
static void print_syscfg(void)
{
    lcd_clear_display();
    lcd_set_foreground(LCD_WHITE);
    line = 0;

    struct SysCfg syscfg;
    const ssize_t result = syscfg_read(&syscfg);

    if (result == -1) {
        printf("SCfg magic not found. NOR flash is corrupted.");
        goto end;
    }

    printf("Total size: %lu bytes, %lu entries", syscfg.header.size, syscfg.header.num_entries);

    if (result > 0) {
        printf("Wrong size: expected %ld, got %lu", result, syscfg.header.size);
    }

    if (syscfg.header.num_entries > SYSCFG_MAX_ENTRIES) {
        printf("Too many entries, showing only first %u", SYSCFG_MAX_ENTRIES);
    }

    const size_t syscfg_num_entries = MIN(syscfg.header.num_entries, SYSCFG_MAX_ENTRIES);

    for (size_t i = 0; i < syscfg_num_entries; i++) {
        const struct SysCfgEntry* entry = &syscfg.entries[i];
        const char* tag = (char *)&entry->tag;
        const uint32_t* data32 = (uint32_t *)entry->data;

        switch (entry->tag) {
        case SYSCFG_TAG_SRNM:
            printf("Serial number (SrNm): %s", entry->data);
            break;
        case SYSCFG_TAG_FWID:
            printf("Firmware ID (FwId): %07lX", data32[1] & 0x0FFFFFFF);
            break;
        case SYSCFG_TAG_HWID:
            printf("Hardware ID (HwId): %08lX", data32[0]);
            break;
        case SYSCFG_TAG_HWVR:
            printf("Hardware version (HwVr): %06lX", data32[1]);
            break;
        case SYSCFG_TAG_CODC:
            printf("Codec (Codc): %s", entry->data);
            break;
        case SYSCFG_TAG_SWVR:
            printf("Software version (SwVr): %s", entry->data);
            break;
        case SYSCFG_TAG_MLBN:
            printf("Logic board serial number (MLBN): %s", entry->data);
            break;
        case SYSCFG_TAG_MODN:
            printf("Model number (Mod#): %s", entry->data);
            break;
        case SYSCFG_TAG_REGN:
            printf("Sales region (Regn): %08lX %08lX", data32[0], data32[1]);
            break;
        default:
            printf("%c%c%c%c: %08lX %08lX %08lX %08lX",
                tag[3], tag[2], tag[1], tag[0],
                data32[0], data32[1], data32[2], data32[3]
            );
            break;
        }
    }

end:
    line++;
    lcd_set_foreground(LCD_RBYELLOW);
    printf("Press SELECT to continue");
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);
}

static void print_bootloader_hash(void)
{
    lcd_clear_display();
    lcd_set_foreground(LCD_WHITE);
    line = 0;

    struct Im3Info hinfo;
    int rc = im3_read(NORBOOT_OFF, &hinfo, NULL);

    if (rc != 0) {
        printf("Error loading the primary bootloader: %d", rc);
        goto end;
    }

    unsigned char primary_hash[SIGN_SZ];

    memcpy(primary_hash, hinfo.u.enc12.data_sign, SIGN_SZ);
    hwkeyaes(HWKEYAES_DECRYPT, HWKEYAES_UKEY, primary_hash, SIGN_SZ);

    unsigned bl_nor_sz = im3_nor_sz(&hinfo);
    rc = im3_read(NORBOOT_OFF + bl_nor_sz, &hinfo, NULL);

    if (rc == 0) {
        // Rockbox bootloader is installed as primary
        // Stock bootloader is backed up
        unsigned char backup_hash[SIGN_SZ];
        memcpy(backup_hash, hinfo.u.enc12.data_sign, SIGN_SZ);
        hwkeyaes(HWKEYAES_DECRYPT, HWKEYAES_UKEY, backup_hash, SIGN_SZ);

        printf("Rockbox bootloader hash:");

        for (int i = 0; i < SIGN_SZ; i++) {
            lcd_putsf(i * 2, line, "%02X", primary_hash[i]);
        }

        line += 2;
        lcd_update();

        printf("Stock bootloader hash:");

        for (int i = 0; i < SIGN_SZ; i++) {
            lcd_putsf(i * 2, line, "%02X", backup_hash[i]);
        }

        line++;
        lcd_update();
    }
    else {
        // Stock bootloader is installed as primary
        // No backup bootloader
        printf("Rockbox bootloader is not installed!");
        line++;

        printf("Stock bootloader hash:");

        for (int i = 0; i < SIGN_SZ; i++) {
            lcd_putsf(i * 2, line, "%02X", primary_hash[i]);
        }

        line++;
        lcd_update();
    }

end:
    line++;
    while (button_status() != BUTTON_NONE)
        sleep(HZ/100);
    lcd_set_foreground(LCD_RBYELLOW);
    printf("Press SELECT to continue");
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);
}

static void menu_dump_onb(void)
{
    lcd_clear_display();
    lcd_set_foreground(LCD_WHITE);
    line = 0;

    printf("=== Dump ONB to disk ===");

    /* Dev bootloader skips storage init, do it here */
    printf("Initializing storage...");
    lcd_update();
    int rc = storage_init();
    if (rc != 0) {
        lcd_set_foreground(LCD_REDORANGE);
        printf("Storage init error: %d", rc);
        goto end;
    }

    filesystem_init();
    rc = disk_mount_all();
    if (rc <= 0) {
        lcd_set_foreground(LCD_REDORANGE);
        printf("No partition found: %d", rc);
        goto end;
    }
    printf("Disk mounted OK");

    rc = dump_onb_to_disk();
    if (rc == 0) {
        lcd_set_foreground(LCD_GREEN);
        piezo_seq(alive);
    } else {
        lcd_set_foreground(LCD_REDORANGE);
        printf("Dump failed: %d", rc);
    }

end:
    line++;
    lcd_set_foreground(LCD_RBYELLOW);
    printf("Press SELECT to continue");
    lcd_update();
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);
}

#ifdef HAVE_SERIAL

#define FLASH_PAGES (FLASH_SIZE >> 12)
#define FLASH_PAGE_SIZE (FLASH_SIZE >> 8)

static void dump_bootflash(void)
{
    lcd_clear_display();
    lcd_set_foreground(LCD_WHITE);
    line = 0;

    uint8_t page[FLASH_PAGE_SIZE];
    printf("Total pages: %d", FLASH_PAGES);

    bootflash_init(SPI_PORT);

    for (int i = 0; i < FLASH_PAGES; i++) {
        printf("Reading flash... %d", i + 1);
        bootflash_read(SPI_PORT, i << 12, FLASH_PAGE_SIZE, page);

        printf("Sending over UART... %d", i + 1);
        serial_tx_raw(page, FLASH_PAGE_SIZE);
        line -= 2;
    }

    bootflash_close(SPI_PORT);

    line += 2;
    printf("Done!");
    piezo_seq(alive);

    line++;
    lcd_set_foreground(LCD_RBYELLOW);
    printf("Press SELECT to continue");
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);
}
#endif /* HAVE_SERIAL */
#endif /* IPOD_6G || IPOD_NANO3G */

static void devel_menu(void)
{
    const char *items[] = {
#ifdef HAVE_LCD_SLEEP
        "LCD sleep/awake test",
#endif
        "PMU info",
        "GPIO info",
#if defined(IPOD_6G) || defined(IPOD_NANO3G)
        "Show SysCfg",
        "Show bootloader hash",
        "Dump ONB to disk",
#ifdef HAVE_SERIAL
        "Dump bootflash to UART",
#endif
#endif
        "Launch OF",
        //"Launch Rockbox",
        "Restart",
        "Power off",
    };
    void (*handlers[])(void) = {
#ifdef HAVE_LCD_SLEEP
        sleep_test,
#endif
        pmu_info,
        gpio_info,
#if defined(IPOD_6G) || defined(IPOD_NANO3G)
        print_syscfg,
        print_bootloader_hash,
        menu_dump_onb,
#ifdef HAVE_SERIAL
        dump_bootflash,
#endif
#endif
        run_of,
        //run_rockbox,
        system_reboot,
        power_off,
    };
    const size_t items_count = sizeof(items) / sizeof(items[0]);
    unsigned char selected_item = 0;

    while (1)
    {
        lcd_clear_display();
        lcd_set_foreground(LCD_RBYELLOW);
        line = 0;
        printf("Development menu");

        for (size_t i = 0; i < items_count; i++) {
            lcd_set_foreground(i == selected_item ? LCD_GREEN : LCD_WHITE);
            printf(items[i]);
        }

        while (button_status() != BUTTON_NONE);

        bool done = false;
        while (!done)
        {
            switch (button_status())
            {
                case BUTTON_MENU:
                case BUTTON_LEFT:
                    if (selected_item > 0) {
                        selected_item--;
                        done = true;
                    }
                    else {
                        sleep(HZ/100);
                    }
                    break;
                case BUTTON_PLAY:
                case BUTTON_RIGHT:
                    if (selected_item < items_count - 1) {
                        selected_item++;
                        done = true;
                    }
                    else {
                        sleep(HZ/100);
                    }
                    break;
                case BUTTON_SELECT:
                    handlers[selected_item]();
                    done = true;
                    break;
                default:
                    sleep(HZ/100);
                    break;
            }
        }
    }
}
#endif /* S5L87XX_DEVELOPMENT_BOOTLOADER */

#ifdef IPOD_6G
static void __attribute__((noreturn)) launch_patched_onb(void *fw_data, int fw_len)
{
    printf("Launching patched ONB...");
    sleep(HZ/2);

    /* Disable IRQ FIRST — im3_read overwrites the exception vector
     * table at IRAM0 (0x22000000). Any interrupt during the SPI read
     * would jump through garbage vectors and crash. */
    disable_irq();

    /* Load ONB from NOR to IRAM0 */
    spi_clkdiv(SPI_PORT, 3);
    struct Im3Info *hinfo = (struct Im3Info *)IRAM1_ORIG;
    im3_read(NORBOOT_OFF + im3_nor_sz(hinfo), hinfo, (void *)IRAM0_ORIG);

    /* Patch ONB: replace arg setup + step 10 BL at 0x105AE-0x105B9
     * (12 bytes exactly). Do NOT touch 0x105BA+ (error exit path).
     * Steps 7-9 (HOB init, MMU+cache, FV HOBs) run normally. */
    uint8_t *onb = (uint8_t *)IRAM0_ORIG;
    static const uint8_t patch[] = {
        0x40, 0xF2, 0x00, 0x00,  /* movw r0, #0x0000 */
        0xC0, 0xF6, 0x00, 0x00,  /* movt r0, #0x0800 */
        0x00, 0x47,              /* bx   r0           */
        0x00, 0xBF,              /* nop (pad to 12)   */
    };
    memcpy(onb + 0x105AE, patch, 12);

    /* Copy firmware to DRAM_ORIG */
    memmove((void *)DRAM_ORIG, fw_data, fw_len);

    eint_init();
    commit_discard_idcache();

    asm volatile("mov pc, %0" :: "r"(IRAM0_ORIG));
    while (1);
}
#endif

void main(void)
{
    int rc = 0;

    usec_timer_init();

#ifdef S5L87XX_DEVELOPMENT_BOOTLOADER
    piezo_seq(alive);
#endif

    /* Configure I2C0 */
    i2c_preinit(0);

    if (pmu_is_hibernated()) {
        rc = launch_onb(1); /* 27/2 = 13.5 MHz. */
    }

    system_preinit();
    memory_init();
    /*
     * XXX: BSS is initialized here, do not use .bss before this line
     */
    bss_init();

    system_init();
    kernel_init();
    i2c_init();
    power_init();

    enable_irq();

#ifdef HAVE_SERIAL
    serial_setup();
#endif

    button_init();
    if (rc == 0) {
        /* User button selection timeout */
        while (USEC_TIMER < 400000);
        int btn = button_read_device();
        /* This prevents HDD spin-up when the user enters DFU */
        if (btn == (BUTTON_SELECT|BUTTON_MENU)) {
            while (button_read_device() == (BUTTON_SELECT|BUTTON_MENU))
                sleep(HZ/10);
            sleep(HZ);
            btn = button_read_device();
        }
        /* Enter OF, diagmode and diskmode using ONB */
        if ((btn == BUTTON_MENU)
                || (btn == (BUTTON_SELECT|BUTTON_LEFT))
                || (btn == (BUTTON_SELECT|BUTTON_PLAY))) {
            rc = kernel_launch_onb();
        }
    }

    lcd_init();
    lcd_set_foreground(LCD_WHITE);
    lcd_set_background(LCD_BLACK);
    lcd_clear_display();
    font_init();
    lcd_setfont(FONT_SYSFIXED);

    // TODO: see if removing this causes the nano3g LCD to initialize properly
#ifdef S5L87XX_DEVELOPMENT_BOOTLOADER
    sleep(HZ);
    for (int i = 0; i < lcd_type+1; i++) {
        sleep(HZ/2);
        piezo_seq(alivelcd);
    }
#endif

    lcd_update();
    sleep(HZ/40);  /* wait for lcd update */

    verbose = true;

    printf("Rockbox boot loader");
    printf("Version: %s", rbversion);

    backlight_init(); /* Turns on the backlight */

#ifdef S5L87XX_DEVELOPMENT_BOOTLOADER
    line++;
    printf("lcd type: %d", lcd_type);
#ifdef S5L_LCD_WITH_READID
    extern unsigned char lcd_id[4];
    uint32_t* lcd_id_32 = (uint32_t *)lcd_id;
    printf("lcd id: 0x%x", *lcd_id_32);
#endif
#ifdef IPOD_NANO4G
    printf("boot cfg: 0x%x", pmu_read(0x7f));
#endif
    line++;
    printf("Press SELECT to continue");
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);

    devel_menu();
#endif /* S5L87XX_DEVELOPMENT_BOOTLOADER */

#ifndef S5L87XX_DEVELOPMENT_BOOTLOADER
    if (rc == 0) {
#if (CONFIG_STORAGE & STORAGE_ATA)
        /* Wait until there is enought power to spin-up HDD */
        battery_trap();
#endif

        rc = storage_init();
        if (rc != 0) {
            printf("Storage error: %d", rc);
            fatal_error(ERR_STORAGE);
        }

        filesystem_init();

        /* We wait until HDD spins up to check for hold button */
        if (button_hold()) {
#ifdef SYSCFG_MAX_ENTRIES
            bool lba48 = false;
            struct SysCfg syscfg;
            const ssize_t result = syscfg_read(&syscfg);
            if (result != -1) {
                const size_t syscfg_num_entries = MIN(syscfg.header.num_entries, SYSCFG_MAX_ENTRIES);
                for (size_t i = 0; i < syscfg_num_entries; i++) {
                    const struct SysCfgEntry* entry = &syscfg.entries[i];
                    const uint32_t* data32 = (uint32_t *)entry->data;
                    if (entry->tag == SYSCFG_TAG_HWVR) {
                        lba48 = (data32[1] >= 0x130200);
                        break;
                    }
                }

                int btn = button_read_device();

                struct storage_info sinfo;
                storage_get_info(0, &sinfo);
                if (sinfo.num_sectors < (1 << 28) || lba48 || btn & BUTTON_LEFT) {
                    printf("Executing OF...");
#if (CONFIG_STORAGE & STORAGE_ATA)
                    ata_sleepnow();
#endif
                    rc = kernel_launch_onb();
                } else {
                    printf("OF does not support LBA48");
                    fatal_error(ERR_LBA28);
                }
            }
#else
            printf("Executing OF...");
#if (CONFIG_STORAGE & STORAGE_ATA)
            ata_sleepnow();
#endif
            rc = kernel_launch_onb();
#endif /* SYSCFG_MAX_ENTRIES */
        }
    }

    if (rc != 0) {
        printf("Load OF error: %d", rc);
        fatal_error(ERR_OF);
    }

#ifdef HAVE_BOOTLOADER_USB_MODE
    /* Enter USB mode if SELECT+RIGHT are pressed */
    if (button_read_device() == (BUTTON_SELECT|BUTTON_RIGHT)) {
#if defined(MAX_VIRT_SECTOR_SIZE) && defined(DEFAULT_VIRT_SECTOR_SIZE)
#ifdef HAVE_MULTIDRIVE
            for (int i = 0 ; i < NUM_DRIVES ; i++)
#endif
                disk_set_sector_multiplier(IF_MD(i,) DEFAULT_VIRT_SECTOR_SIZE/SECTOR_SIZE);
#endif
        usb_mode();
    }
#endif

    rc = disk_mount_all();
    if (rc <= 0) {
#ifdef STORAGE_GET_INFO
        struct storage_info sinfo;
        storage_get_info(0, &sinfo);
#ifdef MAX_PHYS_SECTOR_SIZE
        printf("id: '%s' s:%u*%u", sinfo.product, sinfo.sector_size, sinfo.phys_sector_mult);
#else
        printf("id: '%s' s:%u", sinfo.product, sinfo.sector_size);
#endif
#endif
        struct partinfo pinfo;
        printf("No partition found");
        for (int i = 0 ; i < NUM_VOLUMES ; i++) {
            disk_partinfo(i, &pinfo);
            if (pinfo.type)
                printf("P%d T%02x S%llx",
                       i, pinfo.type, (unsigned long long)pinfo.size);
        }
        fatal_error(ERR_RB);
    }

#ifdef IPOD_6G
    /* Dump ONB from NOR to disk (one-time, triggered by SELECT+LEFT) */
    if (button_read_device() == (BUTTON_SELECT|BUTTON_LEFT)) {
        printf("Dumping ONB...");
        dump_onb_to_disk();
    }

    {
        unsigned char *tmpbuf = (unsigned char *)(DRAM_ORIG + 0x2000000);
        int fwrc = load_raw_firmware(tmpbuf, "/retailos.bin", 12*1024*1024);
        if (fwrc > 0) {
            printf("RetailOS: %d bytes", fwrc);

            /* Compute checksum of patched firmware */
            uint32_t new_chksum = 0;
            for (int i = 0; i < fwrc; i++)
                new_chksum += tmpbuf[i];

            /* Read firmware partition directory to find osos */
            struct storage_info si;
            storage_get_info(0, &si);
            struct partinfo pi;
            disk_partinfo(0, &pi);
            unsigned long fw_sect = pi.start;

            /* Read directory sector at partition + 0x4200/512 */
            unsigned char dirbuf[512];
            storage_read_sectors(fw_sect + 0x4200 / 512, 1, dirbuf);

            /* Find osos entry (scan 40-byte entries) */
            int osos_idx = -1;
            for (int i = 0; i < 512 / 40; i++) {
                uint32_t id;
                memcpy(&id, dirbuf + i * 40 + 4, 4);
                if (id == 0x6F736F73) { /* "osos" LE */
                    osos_idx = i;
                    break;
                }
            }

            if (osos_idx >= 0) {
                uint32_t old_chksum;
                memcpy(&old_chksum, dirbuf + osos_idx * 40 + 28, 4);

                if (old_chksum != new_chksum) {
                    printf("Flashing patched osos...");

                    /* Get devOffset from directory */
                    uint32_t dev_off;
                    memcpy(&dev_off, dirbuf + osos_idx * 40 + 12, 4);
                    unsigned long img_sect = fw_sect + dev_off / 512;

                    /* Write firmware to osos slot */
                    int nsect = (fwrc + 511) / 512;
                    storage_write_sectors(img_sect, nsect, tmpbuf);

                    /* Update directory: len + checksum */
                    uint32_t new_len = fwrc;
                    memcpy(dirbuf + osos_idx * 40 + 16, &new_len, 4);
                    memcpy(dirbuf + osos_idx * 40 + 28, &new_chksum, 4);
                    storage_write_sectors(fw_sect + 0x4200 / 512, 1, dirbuf);

                    printf("Done. Booting OF...");
                } else {
                    printf("osos up to date.");
                }
            }

            /* Boot via ONB — full Apple boot chain */
#if (CONFIG_STORAGE & STORAGE_ATA)
            ata_sleepnow();
#endif
            kernel_launch_onb();
        }
    }
#endif

    printf("Loading Rockbox...");
    unsigned char *loadbuffer = (unsigned char *)DRAM_ORIG;
    rc = load_firmware(loadbuffer, BOOTFILE, MAX_LOADSIZE);

    if (rc <= EFILE_EMPTY) {
        printf("Error!");
        printf("Can't load " BOOTFILE ": ");
        printf(loader_strerror(rc));
        fatal_error(ERR_RB);
    }

    printf("Rockbox loaded.");

    /* If we get here, we have a new firmware image at 0x08000000, run it */
    disable_irq();

    int (*kernel_entry)(void) = (void*)loadbuffer;
    commit_discard_idcache();
    rc = kernel_entry();

    /* End stop - should not get here */
    enable_irq();
    printf("ERR: Failed to boot");
    while(1);
#endif
}
