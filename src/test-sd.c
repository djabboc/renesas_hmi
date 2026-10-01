/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-sd.c
 * @brief SDHI1/TF 卡分级测试及 FatFs 单盘适配。
 *
 * info/read 不写介质；file 使用独占新文件验证持久化读回。
 * 仅 ptest 线程调用；SDHI/DMAC 中断只更新完成事件。
 */
#include "peripheral-test.h"
#include "ff.h"
#include "diskio.h"

#define SD_SECTOR_BYTES 512u
#define SD_TRANSFER_TIMEOUT_MS 2000u
#define SD_TEST_BLOCK_COUNT 16u
#define SD_TEST_FILE_BYTES (SD_SECTOR_BYTES * SD_TEST_BLOCK_COUNT)

static sdmmc_device_t card_info;
static sdmmc_cfg_t sd_config;
static volatile unsigned transfer_events;
static int media_ready;
/* FatFs 用户缓冲不保证四字节对齐，先经过此 DMA 对齐缓冲。 */
static uint8_t sector_buffer[SD_SECTOR_BYTES] BSP_ALIGN_VARIABLE(4);

static void sd_callback(sdmmc_callback_args_t *arguments)
{
    transfer_events |= arguments->event;
}

/* 完成、传输错误和拔卡均结束等待；错误优先于完成位判定。 */
static int wait_for_sd_transfer(void)
{
    rt_tick_t start = rt_tick_get();
    while (!(transfer_events & (SDMMC_EVENT_TRANSFER_COMPLETE | SDMMC_EVENT_TRANSFER_ERROR |
                                SDMMC_EVENT_CARD_REMOVED)) &&
           !test_elapsed(start, SD_TRANSFER_TIMEOUT_MS) && !test_cancelled())
    {
        rt_thread_mdelay(1);
    }
    return (transfer_events & SDMMC_EVENT_TRANSFER_COMPLETE) &&
                   !(transfer_events & (SDMMC_EVENT_TRANSFER_ERROR | SDMMC_EVENT_CARD_REMOVED))
               ? 0
               : -RT_EIO;
}

DSTATUS disk_status(BYTE drive)
{
    sdmmc_status_t status;
    if (drive || !media_ready)
    {
        return STA_NOINIT;
    }
    if (R_SDHI_StatusGet(&g_sdmmc1_ctrl, &status) || !status.card_inserted)
    {
        return STA_NOINIT | STA_NODISK;
    }
    return 0;
}

/* 介质由 test_sd 初始化；FatFs 回调不重复打开控制器。 */
DSTATUS disk_initialize(BYTE drive)
{
    return disk_status(drive);
}

DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector, UINT count)
{
    if (disk_status(drive))
    {
        return RES_NOTRDY;
    }
    if (!count || sector >= card_info.sector_count || count > card_info.sector_count - sector)
    {
        return RES_PARERR;
    }
    for (UINT sector_index = 0; sector_index < count; ++sector_index)
    {
        transfer_events = 0;
        if (R_SDHI_Read(&g_sdmmc1_ctrl, sector_buffer, (uint32_t)sector + sector_index, 1) ||
            wait_for_sd_transfer())
        {
            return RES_ERROR;
        }
        memcpy(buffer + sector_index * SD_SECTOR_BYTES, sector_buffer, SD_SECTOR_BYTES);
    }
    return RES_OK;
}

DRESULT disk_write(BYTE drive, const BYTE *buffer, LBA_t sector, UINT count)
{
    if (disk_status(drive))
    {
        return RES_NOTRDY;
    }
    if (!count || sector >= card_info.sector_count || count > card_info.sector_count - sector)
    {
        return RES_PARERR;
    }
    if (card_info.write_protected)
    {
        return RES_WRPRT;
    }
    for (UINT sector_index = 0; sector_index < count; ++sector_index)
    {
        memcpy(sector_buffer, buffer + sector_index * SD_SECTOR_BYTES, SD_SECTOR_BYTES);
        transfer_events = 0;
        if (R_SDHI_Write(&g_sdmmc1_ctrl, sector_buffer, (uint32_t)sector + sector_index, 1) ||
            wait_for_sd_transfer())
        {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE drive, BYTE command, void *argument)
{
    if (disk_status(drive))
    {
        return RES_NOTRDY;
    }
    switch (command)
    {
    case CTRL_SYNC:
    {
        sdmmc_status_t status;
        rt_tick_t start = rt_tick_get();
        do
        {
            if (R_SDHI_StatusGet(&g_sdmmc1_ctrl, &status))
            {
                return RES_ERROR;
            }
            if (!status.transfer_in_progress)
            {
                return RES_OK;
            }
            rt_thread_mdelay(1);
        } while (!test_elapsed(start, SD_TRANSFER_TIMEOUT_MS) && !test_cancelled());
        return RES_ERROR;
    }
    case GET_SECTOR_COUNT:
        *(LBA_t *)argument = card_info.sector_count;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)argument = SD_SECTOR_BYTES;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)argument = card_info.erase_sector_count;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

/* FAT 日期格式：年份自 1980 起计；测试使用固定日期，不依赖 RTC 阶段。 */
DWORD get_fattime(void)
{
    return ((2026u - 1980) << 25) | (10u << 21) | (1u << 16);
}

/* sector 0 只读两次比对；不把固定签名字节当成必须存在的分区格式。 */
static int verify_sector_read(void)
{
    uint8_t expected_block[SD_SECTOR_BYTES];
    uint8_t actual_block[SD_SECTOR_BYTES];
    if (disk_read(0, expected_block, 0, 1) != RES_OK ||
        disk_read(0, actual_block, 0, 1) != RES_OK ||
        memcmp(expected_block, actual_block, SD_SECTOR_BYTES))
    {
        return -RT_ERROR;
    }
    rt_kprintf("SD sector0 repeat-read matched, signature=%02X%02X\n",
               expected_block[510],
               expected_block[511]);

    return TEST_PASS;
}

/* CREATE_NEW 保证不覆盖用户文件；重新挂载后再比对，避免只验证缓存。
 * FATFS/FIL 局部对象在本函数返回前注销；测试文件保留供电脑复查。
 */
static int verify_file_roundtrip(void)
{
    FATFS filesystem;
    FIL file;
    uint8_t expected_block[SD_SECTOR_BYTES];
    uint8_t actual_block[SD_SECTOR_BYTES];
    char filename[16];
    UINT transferred_bytes;
    int result = -RT_ERROR;
    int file_open = 0;
    FRESULT fatfs_result;
    fatfs_result = f_mount(&filesystem, "0:", 1);
    if (fatfs_result != FR_OK)
    {
        rt_kprintf("SD mount error=%d (FAT16/32 required; never auto-format)\n", fatfs_result);
        goto unmount;
    }
    /* 只申请空闲名字；100 个文件名都占用时返回错误，保留所有旧文件。 */
    for (unsigned file_index = 0; file_index < 100; ++file_index)
    {
        rt_snprintf(filename, sizeof(filename), "0:/HMI%02u.TST", file_index);
        fatfs_result = f_open(&file, filename, FA_CREATE_NEW | FA_WRITE);
        if (fatfs_result != FR_EXIST)
        {
            break;
        }
    }
    if (fatfs_result != FR_OK)
    {
        rt_kprintf("SD create-new error=%d\n", fatfs_result);
        goto unmount;
    }
    file_open = 1;
    rt_kprintf("SD created %s (retained for inspection)\n", filename);
    for (unsigned block = 0; block < SD_TEST_BLOCK_COUNT; ++block)
    {
        for (unsigned byte_index = 0; byte_index < SD_SECTOR_BYTES; ++byte_index)
        {
            expected_block[byte_index] = (uint8_t)(byte_index * 13 + block * 31);
        }
        if (f_write(&file, expected_block, sizeof(expected_block), &transferred_bytes) != FR_OK ||
            transferred_bytes != sizeof(expected_block))
        {
            goto unmount;
        }
    }
    if (f_sync(&file) != FR_OK)
    {
        goto unmount;
    }
    fatfs_result = f_close(&file);
    file_open = 0;
    if (fatfs_result != FR_OK)
    {
        goto unmount;
    }
    f_mount(NULL, "0:", 0);
    if (f_mount(&filesystem, "0:", 1) != FR_OK || f_open(&file, filename, FA_READ) != FR_OK)
    {
        goto unmount;
    }
    file_open = 1;
    if (f_size(&file) != SD_TEST_FILE_BYTES)
    {
        goto unmount;
    }
    for (unsigned block = 0; block < SD_TEST_BLOCK_COUNT; ++block)
    {
        for (unsigned byte_index = 0; byte_index < SD_SECTOR_BYTES; ++byte_index)
        {
            expected_block[byte_index] = (uint8_t)(byte_index * 13 + block * 31);
        }
        if (f_read(&file, actual_block, sizeof(actual_block), &transferred_bytes) != FR_OK ||
            transferred_bytes != sizeof(actual_block) ||
            memcmp(expected_block, actual_block, SD_SECTOR_BYTES))
        {
            goto unmount;
        }
    }
    rt_kprintf("SD file remount/readback 8192 bytes MATCH\n");
    result = 0;
unmount:
    if (file_open && f_close(&file) != FR_OK)
    {
        result = -RT_EIO;
    }
    f_mount(NULL, "0:", 0);
    return result;
}

int test_sd(const char *stage)
{
    sdmmc_status_t status;
    int result = -RT_ERROR;

    if (strcmp(stage, "info") != 0 && strcmp(stage, "read") != 0 && strcmp(stage, "file") != 0)
    {
        return -RT_EINVAL;
    }
    sd_config = g_sdmmc1_cfg;
    sd_config.p_callback = sd_callback;
    media_ready = 0;
    if (R_SDHI_Open(&g_sdmmc1_ctrl, &sd_config) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    if (R_SDHI_StatusGet(&g_sdmmc1_ctrl, &status) != FSP_SUCCESS)
    {
        goto close_controller;
    }
    rt_kprintf("SD card_inserted=%u\n", status.card_inserted);
    if (!status.card_inserted)
    {
        result = TEST_SKIP;
        goto close_controller;
    }
    if (R_SDHI_MediaInit(&g_sdmmc1_ctrl, &card_info) != FSP_SUCCESS)
    {
        goto close_controller;
    }
    media_ready = 1;
    rt_kprintf("SD sectors=%u bytes/sector=%u clock=%u protected=%u\n",
               card_info.sector_count,
               card_info.sector_size_bytes,
               card_info.clock_rate,
               card_info.write_protected);
    if (card_info.sector_size_bytes != SD_SECTOR_BYTES)
    {
        goto close_controller;
    }
    result = TEST_PASS;
    if (strcmp(stage, "info") != 0)
    {
        result = verify_sector_read();
        if (result == TEST_PASS && strcmp(stage, "file") == 0)
        {
            result = verify_file_roundtrip();
        }
    }

close_controller:
    media_ready = 0;
    R_SDHI_Close(&g_sdmmc1_ctrl);
    return result;
}
