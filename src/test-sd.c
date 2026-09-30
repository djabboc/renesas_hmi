/* SPDX-License-Identifier: Apache-2.0 */
#include "peripheral-test.h"
#include "ff.h"
#include "diskio.h"
static sdmmc_device_t card;
static sdmmc_cfg_t cfg;
static volatile unsigned sd_events;
static int media_ready;
static uint8_t sector_buffer[512] BSP_ALIGN_VARIABLE(4);
static void sd_callback(sdmmc_callback_args_t *a) { sd_events |= a->event; }
static int sd_wait(void)
{
    rt_tick_t start=rt_tick_get();
    while (!(sd_events & (SDMMC_EVENT_TRANSFER_COMPLETE|SDMMC_EVENT_TRANSFER_ERROR|SDMMC_EVENT_CARD_REMOVED)) &&
           !test_elapsed(start,2000) && !test_cancelled()) rt_thread_mdelay(1);
    return (sd_events & SDMMC_EVENT_TRANSFER_COMPLETE) && !(sd_events & (SDMMC_EVENT_TRANSFER_ERROR|SDMMC_EVENT_CARD_REMOVED)) ? 0 : -RT_EIO;
}
DSTATUS disk_status(BYTE drive)
{
    sdmmc_status_t status;
    if(drive || !media_ready) return STA_NOINIT;
    if(R_SDHI_StatusGet(&g_sdmmc1_ctrl,&status) || !status.card_inserted) return STA_NOINIT|STA_NODISK;
    return 0;
}
DSTATUS disk_initialize(BYTE drive) { return disk_status(drive); }
DRESULT disk_read(BYTE drive, BYTE *buf, LBA_t sector, UINT count)
{
    if(disk_status(drive)) return RES_NOTRDY;
    if(!count || sector >= card.sector_count || count > card.sector_count-sector) return RES_PARERR;
    for(UINT n=0;n<count;++n) {
        sd_events=0;
        if(R_SDHI_Read(&g_sdmmc1_ctrl,sector_buffer,(uint32_t)sector+n,1) || sd_wait()) return RES_ERROR;
        memcpy(buf+n*512,sector_buffer,512);
    }
    return RES_OK;
}
DRESULT disk_write(BYTE drive, const BYTE *buf, LBA_t sector, UINT count)
{
    if(disk_status(drive)) return RES_NOTRDY;
    if(!count || sector >= card.sector_count || count > card.sector_count-sector) return RES_PARERR;
    if(card.write_protected) return RES_WRPRT;
    for(UINT n=0;n<count;++n) {
        memcpy(sector_buffer,buf+n*512,512);sd_events=0;
        if(R_SDHI_Write(&g_sdmmc1_ctrl,sector_buffer,(uint32_t)sector+n,1) || sd_wait()) return RES_ERROR;
    }
    return RES_OK;
}
DRESULT disk_ioctl(BYTE drive, BYTE cmd, void *arg)
{
    if(disk_status(drive)) return RES_NOTRDY;
    switch(cmd) {
    case CTRL_SYNC: {
        sdmmc_status_t status; rt_tick_t start=rt_tick_get();
        do { if(R_SDHI_StatusGet(&g_sdmmc1_ctrl,&status)) return RES_ERROR;
            if(!status.transfer_in_progress) return RES_OK;
            rt_thread_mdelay(1);
        } while(!test_elapsed(start,2000) && !test_cancelled()); return RES_ERROR;
    }
    case GET_SECTOR_COUNT: *(LBA_t *)arg=card.sector_count;return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)arg=512;return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)arg=card.erase_sector_count;return RES_OK;
    default:return RES_PARERR;
    }
}
DWORD get_fattime(void) { return ((2026u-1980)<<25)|(10u<<21)|(1u<<16); }
int test_sd(const char *stage)
{
    sdmmc_status_t status;
    FATFS fs;
    FIL file;
    uint8_t a[512],b[512];
    char name[16]; UINT count;
    int result=-RT_ERROR,opened=0;
    FRESULT fr;
    if(strcmp(stage,"info") && strcmp(stage,"read") && strcmp(stage,"file")) return -RT_EINVAL;
    cfg=g_sdmmc1_cfg;cfg.p_callback=sd_callback;media_ready=0;
    if(R_SDHI_Open(&g_sdmmc1_ctrl,&cfg)) return -RT_ERROR;
    if(R_SDHI_StatusGet(&g_sdmmc1_ctrl,&status)) goto done;
    rt_kprintf("SD card_inserted=%u\n",status.card_inserted);
    if(!status.card_inserted) { result=TEST_SKIP;goto done; }
    if(R_SDHI_MediaInit(&g_sdmmc1_ctrl,&card)) goto done;
    media_ready=1;
    rt_kprintf("SD sectors=%u bytes/sector=%u clock=%u protected=%u\n",card.sector_count,card.sector_size_bytes,card.clock_rate,card.write_protected);
    if(card.sector_size_bytes!=512) goto done;
    if(!strcmp(stage,"info")) { result=0;goto done; }
    if(disk_read(0,a,0,1)!=RES_OK || disk_read(0,b,0,1)!=RES_OK || memcmp(a,b,512)) goto done;
    rt_kprintf("SD sector0 repeat-read matched, signature=%02X%02X\n",a[510],a[511]);
    if(!strcmp(stage,"read")) { result=0;goto done; }
    fr=f_mount(&fs,"0:",1);
    if(fr!=FR_OK) { rt_kprintf("SD mount error=%d (FAT16/32 required; never auto-format)\n",fr);goto unmount; }
    for(unsigned n=0;n<100;++n) {
        rt_snprintf(name,sizeof(name),"0:/HMI%02u.TST",n);
        fr=f_open(&file,name,FA_CREATE_NEW|FA_WRITE);
        if(fr!=FR_EXIST) break;
    }
    if(fr!=FR_OK) { rt_kprintf("SD create-new error=%d\n",fr);goto unmount; }
    opened=1;rt_kprintf("SD created %s (retained for inspection)\n",name);
    for(unsigned block=0;block<16;++block) {
        for(unsigned i=0;i<512;++i) a[i]=(uint8_t)(i*13+block*31);
        if(f_write(&file,a,sizeof(a),&count)!=FR_OK || count!=sizeof(a)) goto unmount;
    }
    if(f_sync(&file)!=FR_OK) goto unmount;
    fr=f_close(&file);opened=0;if(fr!=FR_OK) goto unmount;
    f_mount(NULL,"0:",0);
    if(f_mount(&fs,"0:",1)!=FR_OK || f_open(&file,name,FA_READ)!=FR_OK) goto unmount;
    opened=1;
    if(f_size(&file)!=8192) goto unmount;
    for(unsigned block=0;block<16;++block) {
        for(unsigned i=0;i<512;++i) a[i]=(uint8_t)(i*13+block*31);
        if(f_read(&file,b,sizeof(b),&count)!=FR_OK || count!=sizeof(b) || memcmp(a,b,512)) goto unmount;
    }
    rt_kprintf("SD file remount/readback 8192 bytes MATCH\n");result=0;
unmount:
    if(opened && f_close(&file)!=FR_OK) result=-RT_EIO;
    f_mount(NULL,"0:",0);
done:
    media_ready=0;R_SDHI_Close(&g_sdmmc1_ctrl);return result;
}
