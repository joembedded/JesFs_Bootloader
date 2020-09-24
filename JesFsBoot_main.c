/*******************************************************************************
* JesFsBoot_main.C: JesFs Bootloader
*
* The Bootloader for JesFs (nRF52 Version)
* based on the Open_Bootloader-Demo
* Written on nRF52840-pca10056 with nRF5_SDK_16.0.0 and SES 4.20a
*
* Watchog: After Power-On Reset or Watchdog-Reset the Watchdog 
* is disabled, Reset with NVIC_SystemReset() keeps Watchdog running,
* JesFsBoot always enables the Watchdog with a Timout of >= 250 secs.
*
* (C)2020 joembedded@gmail.com - www.joembedded.de
* Version: 
* 1.00 / 11.01.2020
* 1.02 / 22.02.2020 - Fixed error with incomplete Firmware Files
* 1.5 / 08.09.2020 - SDK17 and SES 4.52b
* 1.51 / 14.09.2020 - Cosmetics
* 1.52 / 24.09.2020 - SDK17.0.2
*******************************************************************************/

#define VERSION "1.52 / 24.09.2020"

#include "boards.h"
#include "nrf_bootloader.h"
#include "nrf_bootloader_app_start.h"
#include "nrf_mbr.h"
#include <stdint.h>

#include "app_error.h"
#include "app_error_weak.h"
#include "nrf_bootloader_info.h"
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"

#include "app_timer.h"
#include "nrf_clock.h"
#include "nrf_delay.h"

#include "nrf_nvmc.h"
#include "nrf_wdt.h"

//======= Toolbox =======
#include "tb_tools.h"

//===== JesFs =====
#include "jesfs.h"
#include "jesfs_int.h" // fs_track_crc32()

//=========== Formating the Flash may take up to 240 secs =======
#ifdef PLATFORM_NRF52
#if NRFX_WDT_CONFIG_RELOAD_VALUE < 250000
#warning "Watchdog Interval < 250 seconds"
#endif
#endif

//====================== Globals ===============

//#define CMDL_DEBUG // defined for Development

// Header Type0 of JesFsHex2Bin FILE
#define HDR0_MAGIC 0xE79B9C4F
typedef struct {
    uint32_t hdrmagic;     // 0 MagicHeader Type0: HDR0_MAGIC
    uint32_t hdrsize;      // 1 Size in Bytes (Type0: 32 for 8 uint32)
    uint32_t binsize;      // 2 Size of following BinaryBlock
    uint32_t binload;      // 3 Adr0 of following BinaryBlock
    uint32_t crc32;        // 4 CRC32 of following BinaryBlock
    uint32_t timestamp;    // 5 UnixSeconds of this file
    uint32_t binary_start; // 6 StartAddress Binary (Parameter 2 of 'h')
    uint32_t resv0;        // 7 Reserved, 0xFFFFFFFF
} HDR0_TYPE;

HDR0_TYPE hdr_buf;

HDR0_TYPE *pbl_memory = (HDR0_TYPE *)BOOTLOADER_SETTINGS_ADDRESS;

#define MAX_INPUT 80
char input[MAX_INPUT + 1]; // 0 at end

#define SBUF_SIZE CODE_PAGE_SIZE // Buffer to hold >= 1 CPU Flash Page
uint8_t sbuffer[SBUF_SIZE];      // Test buffer

uint32_t mac_addr_h,mac_addr_l; 

//==== JesFs globals =====
FS_DESC fs_desc;
FS_STAT fs_stat;
FS_DATE fs_date; // Structe holding date-time

//============= functions =================
static void on_error(void) {
    NRF_LOG_FINAL_FLUSH();

#if NRF_MODULE_ENABLED(NRF_LOG_BACKEND_RTT)
    // To allow the buffer to be flushed by the host.
    nrf_delay_ms(100);
#endif
#ifdef NRF_DFU_DEBUG_VERSION
    NRF_BREAKPOINT_COND;
#endif
    NVIC_SystemReset();
}

void app_error_handler(uint32_t error_code, uint32_t line_num, const uint8_t *p_file_name) {
    NRF_LOG_ERROR("app_error_handler err_code:%d %s:%d\n", error_code, p_file_name, line_num);
    on_error();
}

void app_error_fault_handler(uint32_t id, uint32_t pc, uint32_t info) {
    NRF_LOG_ERROR("Received a fault! id: 0x%08x, pc: 0x%08x, info: 0x%08x\n", id, pc, info);
    on_error();
}

void app_error_handler_bare(uint32_t error_code) {
    NRF_LOG_ERROR("Received an error: 0x%08x!\n", error_code);
    on_error();
}

//=========== Helper Functions ===============
//=== Platform specific ===
uint32_t _time_get(void) {
    return tb_time_get();
}

//=== common helpers ===
#ifdef CMDL_DEBUG
// Helper Function for readable timestamps
void conv_secs_to_date_sbuffer(uint32_t secs) {
    fs_sec1970_to_date(secs, &fs_date);
    sprintf((char *)sbuffer, "%02u.%02u.%04u %02u:%02u:%02u", fs_date.d, fs_date.m, fs_date.a, fs_date.h, fs_date.min, fs_date.sec);
}
#endif

// Init system and Toolbox
void bootloader_init(void) {
    uint32_t ret_val;

    // Must happen before flash protection is applied, since it edits a protected page.
    nrf_bootloader_mbr_addrs_populate();

    // Protect MBR and bootloader code from being overwritten.
    ret_val = nrf_bootloader_flash_protect(0, MBR_SIZE /*, false(only SDK16)*/);
    APP_ERROR_CHECK(ret_val);
    ret_val = nrf_bootloader_flash_protect(BOOTLOADER_START_ADDR, BOOTLOADER_SIZE /*, false (only SDK16)*/);
    APP_ERROR_CHECK(ret_val);

    ret_val = NRF_LOG_INIT(app_timer_cnt_get);
    APP_ERROR_CHECK(ret_val);
    NRF_LOG_DEFAULT_BACKENDS_INIT();

    // Init Toolbox
    tb_init();
    tb_watchdog_init(); // WATCHDOG Channel0 ON
}
//--- check_system() -------
int16_t check_system(void) {
    bool cpu_firmware_valid = false;
    int16_t res;
    int32_t lres;
    int32_t total_len, block_len;
    uint32_t fadr; // Adr in Flash
    uint32_t ures;

    APP_ERROR_CHECK(sizeof(HDR0_TYPE) != 32);
    // 1.) Self-Check (CRC of Bootloader er OK?)
    // ToDo

    // 2.) Check, if APP is present ans Bootloader has stored it:
#ifdef CMDL_DEBUG
    tb_printf("Bootloader Start: 0x%X (Codesize %d of max. %d)\n",CODE_START, CODE_SIZE,BOOTLOADER_SIZE);
    tb_printf("BootloaderSettingsPage (%x):\n", BOOTLOADER_SETTINGS_ADDRESS);
    tb_printf("-Magic: %x\n", pbl_memory->hdrmagic);  // == HDR0_MAGIC
    tb_printf("-HdrSize: %x\n", pbl_memory->hdrsize); // File header (==32)
    tb_printf("-BinSize: %x\n", pbl_memory->binsize); // == Rest of File
    tb_printf("-BinLoad: %x\n", pbl_memory->binload);
    tb_printf("-CRC32: %x\n", pbl_memory->crc32);
    tb_printf("-Timestamp: %x\n", pbl_memory->timestamp);
    tb_printf("-(BinStart: %x)\n", pbl_memory->binary_start);
    tb_printf("-(Resv0: %x)\n", pbl_memory->resv0);
#endif

    if (pbl_memory->hdrmagic == HDR0_MAGIC && pbl_memory->hdrsize == sizeof(HDR0_TYPE)) {
        // Use JesFs CRC fkt. to check integrity of CPU firmware
        ures = fs_track_crc32((uint8_t *)pbl_memory->binload, pbl_memory->binsize, 0xFFFFFFFF);
        if (ures == pbl_memory->crc32) {
            tb_printf("Valid Firmware on CPU\n");
            cpu_firmware_valid = true;
        }
    }

    // 3.) Check, if Firmware File is present and if OK: Flash it
    res = fs_open(&fs_desc, "_firmware.bin", SF_OPEN_READ | SF_OPEN_CRC);
    if (res == 0) { // File Found
        lres = fs_read(&fs_desc, (uint8_t *)&hdr_buf, sizeof(hdr_buf));
        if (lres != 32 || hdr_buf.hdrmagic != HDR0_MAGIC) {
            if(cpu_firmware_valid) return -101; // Firmwarefile defect, but old Firmware OK
            return -1;
        }
        if (hdr_buf.timestamp == pbl_memory->timestamp && hdr_buf.crc32 == pbl_memory->crc32 && cpu_firmware_valid) {
            return 0; // Firmware on CPU identical to File
        }
        tb_printf("Found different '_firmware.bin'.\nCheck: ");

        // First check integrity of new firmware by reading it, indirect use
        fs_desc.file_crc32 = 0xFFFFFFFF; // reset CRC
        total_len = hdr_buf.binsize;
        while (total_len) {
            tb_putc('*'); // Check
            block_len = total_len;
            if (block_len > sizeof(sbuffer))
                block_len = sizeof(sbuffer);
            lres = fs_read(&fs_desc, sbuffer, block_len);
            if (lres != block_len) {
                if(cpu_firmware_valid) return -102; // Firmwarefile defect, but old Firmware OK
                return -2; // File Read Error/Len
            }
            total_len -= block_len;
        }
        // Now fs_desc.file_crc32 should contain correct CRC
        if (fs_desc.file_crc32 != hdr_buf.crc32) {
            if(cpu_firmware_valid) return -103; // Firmwarefile defect, but old Firmware OK
            return -3; // firmeware.bin CRC32 corrupt
        }
        tb_printf("\nFlash: ");
        fs_rewind(&fs_desc); // Rewind Firmware File to first Codebyte
        fs_read(&fs_desc, NULL, sizeof(hdr_buf));

        total_len = hdr_buf.binsize;
        fadr = hdr_buf.binload;
        while (total_len) {
            tb_putc('*'); // Progress
            block_len = total_len;
            if (block_len > sizeof(sbuffer))
                block_len = sizeof(sbuffer);
            lres = fs_read(&fs_desc, sbuffer, block_len);
            if (lres != block_len) {
                return -4; // Maybe still alive..
            }

            nrf_nvmc_page_erase(fadr);
            nrf_nvmc_write_words(fadr, (uint32_t *)sbuffer, (block_len + 3) / 4);

            total_len -= block_len;
            fadr += block_len;
        }
        // Last step: Write Bootloader_Settings_Page
        nrf_nvmc_page_erase(BOOTLOADER_SETTINGS_ADDRESS);
        nrf_nvmc_write_words(BOOTLOADER_SETTINGS_ADDRESS, (uint32_t *)&hdr_buf, sizeof(hdr_buf) / 4);

        // Step 3b.) Re-Check if new Firmware is OK in the CPU. Redo CRC step from above
        if (pbl_memory->hdrmagic == HDR0_MAGIC && pbl_memory->hdrsize == sizeof(HDR0_TYPE)) {
            // Use JesFs CRC fkt. to check integrity of CPU firmware
            ures = fs_track_crc32((uint8_t *)pbl_memory->binload, pbl_memory->binsize, 0xFFFFFFFF);
            if (ures == pbl_memory->crc32) {
                tb_printf("\nFlashed and Verified.\n");
                return 1;
            }
        }
        return -5; // Verify failed!
    }
    
    if(!cpu_firmware_valid) { // Error <= -100
      return -206;  // No Valid Firmware on CPU and no valid firmware.bin...
    }
    return 0;
}

/**====MAIN======= */
void main(void) {
    char *pc;
    uint32_t uval;
    int32_t res;
    int32_t i, j;
    uint32_t adr;
    uint32_t val;

    mac_addr_h = NRF_FICR->DEVICEADDR[1]; // Regs. locked by SD
    mac_addr_l = NRF_FICR->DEVICEADDR[0];
 
    bootloader_init();

    NRF_LOG_INFO("JesFsBoot started\n");
    NRF_LOG_FLUSH();

    tb_printf("\n*** JesFsBoot (No Encryption) " VERSION " (C)2020 JoEmbedded.de\n\n");
    
    tb_printf("MAC:%08X%08X\n", mac_addr_h, mac_addr_l);

#ifdef CMDL_DEBUG
    tb_printf("*DEBUG*\n");
#endif

    res = fs_start(FS_START_NORMAL);
    if (res)  // 
        tb_printf("Filesystem ERROR:%d\n", res);
    else
        tb_printf("Filesystem OK\n");

    tb_printf("Disk size: %d Bytes\n", sflash_info.total_flash_size);

    tb_printf("Bootloader Start: 0x%X (Codesize %d of max. %d)\n",CODE_START, CODE_SIZE,BOOTLOADER_SIZE);

    res = check_system();
    if (res < 0) { // Unexpected Return
        // <= - 200: NOTHING valid
        if(res<=-200) tb_printf("\nERROR: No Firmware found (%d)\n", res);
        else {
          // <0 and >-199: Firmware-File corrupt
          tb_printf("\nERROR: '_firmware.bin' corrupt (%d)\n", res);
          // <=-100 and >-199: ..But old Firmware present!
          if(res<=-100){
            tb_printf("...Firmware not changed!\n");
#ifndef CMDL_DEBUG
            i=5;    // Blink 5 times if update failed
            while(i--){
              tb_printf("Restart old Firmware(%u)\n",i);
              tb_board_led_invert(0); // 10 sec Hektisch blinken
              tb_delay_ms(100);
              tb_board_led_invert(0);
              tb_delay_ms(100);
              tb_board_led_invert(0);
              tb_delay_ms(800);
            }
            res=2;
#endif
          }
        }
    }

#ifndef CMDL_DEBUG
    if(res<0){  // 
        i=60;
        while(i--){
          tb_board_led_invert(0);
          tb_printf("ERROR: %d, Wait for Reboot(%u)\n",res,i);
          tb_delay_ms(1000);
        }
        NVIC_SystemReset();

    }else{  // Everything is OK: Start USER
         fs_deepsleep();
         tb_printf("Start Firmware...\n");
         tb_delay_ms(5); // OK for 50 Bytes
         tb_uninit();
         nrf_bootloader_app_start();
         // Never come back 
    }
#else    
/******************************************************************************
* For Development JesFsBootloader has a CLI. 
* ToDo: Add Crypto, add other file access like UART, USB, BLE-light, ...
******************************************************************************/

    for (;;) {
        tb_board_led_on(0);

        tb_time_get();                             // Dummy call to update Unix-Timer
        tb_printf("> ");                           // Show prompt
        res = tb_gets(input, MAX_INPUT, 60000, 1); // 60 seconds time with echo
        tb_putc('\n');
        tb_watchdog_feed(1); // Now 250 secs time
        tb_board_led_off(0);

        if (res > 0) {      // ignore empty lines
            pc = &input[1]; // point to 1.st argument
            while (*pc == ' ')
                pc++;                    // Remove Whitspaces from Input
            uval = strtoul(pc, NULL, 0); // Or 0

            switch (input[0]) {

            case 'F':         // Format Disk
                i = atoi(pc); // Full:1 (Hardware Chip Erase) Soft Erase :2 (faster if not full)
                pc = "???";
                if (i == 1)
                    pc = "Chip Erase";
                else if (i == 2)
                    pc = "Soft Erase";
                tb_printf("'F' Format Serial Flash (Mode:%d(%s)) (may take up to 240 secs!)...\n", i, pc);
                tb_printf("FS format: Res:%d\n", fs_format(i));
                break;

            case 'v': // Listing on virtual Disk - Only basic infos. No checks, Version with **File Health-Check** for all files with CRC in JesFs_cmd.c
                tb_printf("'v' Directory:\n");
                tb_printf("Disk size: %d Bytes\n", sflash_info.total_flash_size);
                if (sflash_info.creation_date == 0xFFFFFFFF) { // Severe Error
                    tb_printf("Error: Invalid/Unformated Disk!\n");
                    break;
                }
                tb_printf("Disk available: %d Bytes / %d Sectors\n", sflash_info.available_disk_size, sflash_info.available_disk_size / SF_SECTOR_PH);
                conv_secs_to_date_sbuffer(sflash_info.creation_date);
                tb_printf("Disk formated [%s]\n", sbuffer);
                for (i = 0; i < sflash_info.files_used + 1; i++) { // Mit Testreserve
                    res = fs_info(&fs_stat, i);

                    if (res <= 0)
                        break;
                    if (res & FS_STAT_INACTIVE)
                        tb_printf("(- '%s'   (deleted))\n", fs_stat.fname); // Inaktive/Deleted
                    else if (res & FS_STAT_ACTIVE) {
                        tb_printf("- '%s'   ", fs_stat.fname); // Active
                        if (res & FS_STAT_UNCLOSED) {
                            fs_open(&fs_desc, fs_stat.fname, SF_OPEN_READ | SF_OPEN_RAW); // Find out len by DummyRead
                            fs_read(&fs_desc, NULL, 0xFFFFFFFF);                          // Read as much as possible
                            fs_close(&fs_desc);                                           // Prevent descriptor from Reuse
                            tb_printf("(Unclosed: %u Bytes)", fs_desc.file_len);
                        } else {
                            tb_printf("%u Bytes", fs_stat.file_len);
                        }
                        // The creation Flags
                        if (fs_stat.disk_flags & SF_OPEN_CRC)
                            tb_printf(" CRC32:%x", fs_stat.file_crc32);
                        if (fs_stat.disk_flags & SF_OPEN_EXT_SYNC)
                            tb_printf(" ExtSync");
                        //if(fs_stat.disk_flags & _SF_OPEN_RES) tb_printf(" Reserved");
                        conv_secs_to_date_sbuffer(fs_stat.file_ctime);
                        tb_printf(" [%s]\n", sbuffer);
                    }
                }
                tb_printf("Disk Nr. of files active: %d\n", sflash_info.files_active);
                tb_printf("Disk Nr. of files used: %d\n", sflash_info.files_used);
#ifdef JSTAT
                if (sflash_info.sectors_unknown)
                    tb_printf("WARNING - Found %d Unknown Sectors\n", sflash_info.sectors_unknown);
#endif
                tb_printf("Res:%d\n", res);
                break;

            case 'V': // Run Careful Disk Check with opt. Output, requires a temp. buffer
                fs_check_disk(tb_printf, sbuffer, SBUF_SIZE);
                break;

            case 'M': // mADR Examine Memory Adr. in Hex (CPU-Flash)
                // Read 1 page of the serial flash (internal function)
                adr = strtoul(pc, 0, 16); // Hex
                tb_printf("CPU Memory 0x%06x:\n", adr);
                for (i = 0; i < 16; i++) {
                    tb_printf("%06X: ", adr + i * 16);
                    for (j = 0; j < 16; j++)
                        tb_printf("%02X ", ((uint8_t *)(adr))[i * 16 + j]);
                    for (j = 0; j < 16; j++) {
                        res = ((uint8_t *)(adr))[i * 16 + j];
                        if (res < ' ' || res > 127)
                            res = '.';
                        tb_printf("%c", res);
                    }
                    tb_printf("\n");
                }
                break;

            case 'B':
                fs_deepsleep();
                tb_printf("Boot(User)\n");
                tb_uninit();
                nrf_bootloader_app_start();
                break;

            case 'W': // Write Longword to Memory
                adr = strtol(pc, &pc, 16);
                val = strtoul(pc, 0, 16);
                tb_printf("Write @Memory %06X: %X\n", adr, val);
                nrf_nvmc_write_word(adr, val);
                if (*(uint32_t *)adr != val) {
                    tb_printf("VERIFY ERROR! (%X)\n", *(uint32_t *)adr);
                } else {
                    tb_printf("OK\n");
                }
                break;
            case 'E': // Erase CPU-Page
                adr = strtol(pc, 0, 16);
                tb_printf("Erase @MemoryPage %06X\n", adr);
                nrf_nvmc_page_erase(adr);
                tb_printf("OK\n");
                break;

            default:
                tb_printf("???\n");
                break;
            }
        }
    }
#endif
}

//***