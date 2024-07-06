/*
 * Copyright (c) 2024 Jakub Zimnol
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <RP2040.h>
#include <hardware/flash.h>
#include <hardware/resets.h>
#include <hardware/sync.h>
#include <pico/stdlib.h>

#include <pico_fota_bootloader.h>

#include "port_common.h"
#include "wizchip_conf.h"
#include "w5x00_spi.h"
#include "socket.h"
#include "dhcp.h"


#include "linker_common/linker_definitions.h"

#ifdef PFB_WITH_BOOTLOADER_LOGS
#    define BOOTLOADER_LOG(...)                \
        do {                                   \
            puts("[BOOTLOADER] " __VA_ARGS__); \
            sleep_ms(5);                       \
        } while (0)
#else // PFB_WITH_BOOTLOADER_LOGS
#    define BOOTLOADER_LOG(...) ((void) 0)
#endif // PFB_WITH_BOOTLOADER_LOGS

void _pfb_mark_pico_has_new_firmware(void);
void _pfb_mark_pico_has_no_new_firmware(void);
void _pfb_mark_is_after_rollback(void);
void _pfb_mark_is_not_after_rollback(void);
bool _pfb_should_rollback(void);
void _pfb_mark_should_rollback(void);
bool _pfb_has_firmware_to_swap(void);
uint32_t _pfb_firmware_swap_size(void);

static void swap_images(void) {
    uint8_t swap_buff_from_downlaod_slot[FLASH_SECTOR_SIZE];
    uint8_t swap_buff_from_application_slot[FLASH_SECTOR_SIZE];
    uint32_t swap_size = _pfb_firmware_swap_size();
    if (swap_size == 0 || swap_size > PFB_ADDR_AS_U32(__FLASH_SWAP_SPACE_LENGTH)) swap_size = PFB_ADDR_AS_U32(__FLASH_SWAP_SPACE_LENGTH);
    printf("SWAPPING %d bytes\n",swap_size);
    const uint32_t SWAP_ITERATIONS = swap_size / FLASH_SECTOR_SIZE;

    uint32_t saved_interrupts = save_and_disable_interrupts();
    for (uint32_t i = 0; i < SWAP_ITERATIONS; i++) {
        memcpy(swap_buff_from_downlaod_slot,
               (void *) (PFB_ADDR_AS_U32(__FLASH_DOWNLOAD_SLOT_START)
                         + i * FLASH_SECTOR_SIZE),
               FLASH_SECTOR_SIZE);
        memcpy(swap_buff_from_application_slot,
               (void *) (PFB_ADDR_AS_U32(__FLASH_APP_START)
                         + i * FLASH_SECTOR_SIZE),
               FLASH_SECTOR_SIZE);
        flash_range_erase(PFB_ADDR_WITH_XIP_OFFSET_AS_U32(__FLASH_APP_START)
                                  + i * FLASH_SECTOR_SIZE,
                          FLASH_SECTOR_SIZE);
        flash_range_erase(PFB_ADDR_WITH_XIP_OFFSET_AS_U32(
                                  __FLASH_DOWNLOAD_SLOT_START)
                                  + i * FLASH_SECTOR_SIZE,
                          FLASH_SECTOR_SIZE);
        flash_range_program(PFB_ADDR_WITH_XIP_OFFSET_AS_U32(__FLASH_APP_START)
                                    + i * FLASH_SECTOR_SIZE,
                            swap_buff_from_downlaod_slot,
                            FLASH_SECTOR_SIZE);
        flash_range_program(PFB_ADDR_WITH_XIP_OFFSET_AS_U32(
                                    __FLASH_DOWNLOAD_SLOT_START)
                                    + i * FLASH_SECTOR_SIZE,
                            swap_buff_from_application_slot,
                            FLASH_SECTOR_SIZE);
    }
    restore_interrupts(saved_interrupts);
}

static void disable_interrupts(void) {
    SysTick->CTRL &= ~1;

    NVIC->ICER[0] = 0xFFFFFFFF;
    NVIC->ICPR[0] = 0xFFFFFFFF;
}

static void reset_peripherals(void) {
    reset_block(~(RESETS_RESET_IO_QSPI_BITS | RESETS_RESET_PADS_QSPI_BITS
                  | RESETS_RESET_SYSCFG_BITS | RESETS_RESET_PLL_SYS_BITS));
}

static void jump_to_vtor(uint32_t vtor) {
    // Derived from the Leaf Labs Cortex-M3 bootloader.
    // Copyright (c) 2010 LeafLabs LLC.
    // Modified 2021 Brian Starkey <stark3y@gmail.com>
    // Originally under The MIT License

    uint32_t reset_vector = *(volatile uint32_t *) (vtor + 0x04);
    SCB->VTOR = (volatile uint32_t)(vtor);

    asm volatile("msr msp, %0" ::"g"(*(volatile uint32_t *) vtor));
    asm volatile("bx %0" ::"r"(reset_vector));
}

static void print_welcome_message(void) {
#ifdef PFB_WITH_BOOTLOADER_LOGS
    puts("");
    puts("***********************************************************");
    puts("*                                                         *");
    puts("*           Raspberry Pi Pico W FOTA Bootloader           *");
    puts("*        Base code copyright (c) 2024 Jakub Zimnol        *");
    puts("*       HTTP fallback recover (c) 2024 Glenn Dickins      *");
    puts("*                                                         *");
    puts("***********************************************************");
    puts("");
#endif // PFB_WITH_BOOTLOADER_LOGS
}

///////////////////////////////////////////////////////////////////////////////////////////////////////
// HTML FOR RECOVERY PAGE
//
// A bit of info, and most importantly a file upload form using a POST.
//
static char page_recover[] = 
"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\"><title>DA Dongle</title></head><body>"
"<h1>SYSTEM RECOVERY</h1>"
"Bootloader interrupted.  Would be running non-committed or rollback firmware.<br><br>"
"If device is no longer booting, it is likely that power was interrupted<br>"
"during the last device firmware update or rollback.<br><br>"
"A new firmware can be loaded here.  This will take about 2 minutes.<br><br>"
"New firmware should boot successfully, after which refresh this page or<br>"
"return to main pages.<br><br>"
"<input type=\"file\" id=\"input\" onchange=\"upload()\"><br><br>"
"  <script>"
"      function upload() {"
"          const input = document.getElementById('input');"
"          if (input.files.length > 0) {"
"              const rdr = new FileReader();"
"              rdr.onload = e => fetch('upload', {"
"                  method: 'POST',"
"                  headers: {'Content-Type': 'application/octet-stream'},"
"                  body: e.target.result"
"              }).then(res => res.text()).catch(err => console.error('Error:', err));"
"              rdr.readAsArrayBuffer(input.files[0]);"
"          }"
"      }"
"  </script>"
"</body></html>";
    


int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    print_welcome_message();

    #define WAIT (2000000)
    if (_pfb_should_rollback() || _pfb_has_firmware_to_swap() || pfb_is_after_firmware_update() || pfb_is_after_rollback())
    {

        puts("RUNNING A RECOVERY MINIMAL WEB SERVER");

        wizchip_spi_initialize();           // NOTE MAKE SURE TO PATCH THIS TO BE 36Mhz not 5Mhz SPI
        wizchip_reset();
        wizchip_initialize();               // NOTE This routine will wait for a PHY link
        wizchip_check();

        static uint8_t g_ethernet_buf[2048] = {};
        wiz_NetInfo net_info = { .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56},
                                .ip = {192, 168, 0, 100},
                                .sn = {255, 255, 255, 0},
                                .gw = {192, 168, 0,  1},
                                .dns = {8, 8, 8, 8},
                                .dhcp = NETINFO_STATIC };

        puts("ATTEMPTING DHCP");

        int wait=0;
        for (int tries=0; tries<10; tries++)
        {
            puts("ATTEMPT");
            wait = 20;
            DHCP_init(1, g_ethernet_buf);       // Use socket 1
            for (; wait>0; wait--) 
            {
                if (DHCP_run() == DHCP_IP_LEASED) break;
                sleep_ms(100);
            }
            DHCP_stop();
            if (wait>0) break;
        }

        if (wait==0)                                                // And if that fails, use the default zero config using the unique id
        {
            network_initialize(net_info);
        }

        ctlnetwork(CN_GET_NETINFO, (void *)&net_info);

        printf("IP ADDRESS        %d.%d.%d.%d\n",   net_info.ip[0], net_info.ip[1], net_info.ip[2], net_info.ip[3]);

        int64_t start = time_us_64();
        bool    contact = false;
        while( (time_us_64()-start < WAIT) || contact)
        {
            printf("Waiting for a connection...\n");
            socket(1, Sn_MR_TCP, 80,0x00);
            listen(1);
            int wait = time_us_64();
            while (getSn_RX_RSR(1)==0 && (time_us_64()-wait < 1000000) ) sleep_ms(10);
            int len = getSn_RX_RSR(1);
            if (len==0) continue;
            printf("Connection received\n");
            if (len>(int)sizeof(g_ethernet_buf)) len = sizeof(g_ethernet_buf)-1;
            len = recv(1, g_ethernet_buf, len);
            g_ethernet_buf[len] = 0;
            if (strstr((char *)g_ethernet_buf, "GET /recover") != NULL)     
            {
                send(1, (uint8_t *)page_recover, sizeof(page_recover));
                printf("Sent page\n");
                sleep_ms(100);
                setSn_CR(1,Sn_CR_DISCON);        // A healthy disconnect
                sleep_ms(100);
                close(1);
                contact = true;
            }
            else if (strstr((char *)g_ethernet_buf, "POST") != NULL)     
            {
                char *data = strstr((char *)g_ethernet_buf, "\r\n\r\n") + 4;
                len = len - ((int32_t)data - (int32_t)g_ethernet_buf);
                printf("POST got %d bytes\n",len);
                printf("Initializing download slot\n");
                pfb_initialize_download_slot(0);

                int received = len;
                int upload_done = 0;

                static char upload_buffer[PFB_ALIGN_SIZE];
                static int  upload_pos=0;
                
                while(len>0)
                {
                    while (len && (upload_pos<(int)sizeof(upload_buffer))) 
                    {
                        upload_buffer[upload_pos++] = *data++;
                        len--;
                    }

                    if (upload_pos==sizeof(upload_buffer))
                    {
                        int ret = pfb_write_to_flash_aligned_256_bytes((uint8_t*)upload_buffer, upload_done, upload_pos);
                        if (ret) printf("ERROR LOADING FIRMWARE\n");
                        upload_done += upload_pos;
                        upload_pos = 0;
                    }

                    if (len==0)
                    {
                        len = getSn_RX_RSR(1);
                        if (len>0)
                        {
                            if (len>(int)sizeof(g_ethernet_buf)) len = sizeof(g_ethernet_buf)-1;
                            len = recv(1, g_ethernet_buf, len);
                            data = (char *)g_ethernet_buf;
                            received += len;
                            printf("Received %d bytes   total %d\n", len, received);
                        }
                    }
                }
                // Will end when the socket closes or there is no more data coming
                printf("Firmware flash complete  DONE %d\n",upload_done);
                int ret_sha256 = pfb_firmware_sha256_check(upload_done);
                if (ret_sha256) printf("FAILED THE SHA TEST\n");
                else
                {
                    printf("SHA PASSED GETTING READY TO REBOOT AND UPDATE!!!!\n");
                    sleep_ms(100);
                    pfb_mark_download_slot_as_valid(0);
                    pfb_perform_update();
                    
                }
                close(1);
            }
        }
    }

    if (_pfb_should_rollback()) {
        BOOTLOADER_LOG("Rolling back to the previous firmware");
        swap_images();
        pfb_firmware_commit();
        _pfb_mark_pico_has_no_new_firmware();
        _pfb_mark_is_after_rollback();
    } else if (_pfb_has_firmware_to_swap()) {
        BOOTLOADER_LOG("Swapping images");
        swap_images();
        _pfb_mark_pico_has_new_firmware();
        _pfb_mark_is_not_after_rollback();
        _pfb_mark_should_rollback();
    } else {
        BOOTLOADER_LOG("Nothing to swap");
        pfb_firmware_commit();
        _pfb_mark_pico_has_no_new_firmware();
    }

    pfb_mark_download_slot_as_invalid();
    BOOTLOADER_LOG("End of execution, executing the application...\n");

    disable_interrupts();
    reset_peripherals();
    jump_to_vtor(__flash_info_app_vtor);

    return 0;
}
