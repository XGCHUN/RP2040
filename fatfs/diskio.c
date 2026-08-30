/*-----------------------------------------------------------------------*/
/* Low level disk I/O forwarding layer for FatFs                         */
/*-----------------------------------------------------------------------*/
/*
 * Part of grblHAL.
 *
 * This used to contain the platform SD card block driver directly (ChaN's
 * MMC/SDC-over-SPI sample, adapted for RP2040/RP2350). It is now a thin
 * forwarding layer: a block-device backend registers a diskio_ops_t via
 * diskio_register() (see diskio_register.h) during hardware init, and the
 * FatFs disk_* entry points below forward to it.
 *
 * Backends live in their own files, e.g.:
 *   - sdcard/sd_spi.c   SD card over SPI  (SDCARD_ENABLE && !SDCARD_SDIO)
 *   - sdcard/sdio.c     SD card over PIO SDIO (SDCARD_ENABLE && SDCARD_SDIO)
 *
 * Adding or swapping a backend no longer requires editing this file.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "driver.h"

#include "ff.h"
#include "diskio.h"
#include "diskio_register.h"
#include "pico/stdlib.h"

/*-----------------------------------------------------------------------*/
/* Registration table                                                    */
/*-----------------------------------------------------------------------*/

static const diskio_ops_t *drive_ops[DISKIO_MAX_DRIVES] = { 0 };

void diskio_register (BYTE pdrv, const diskio_ops_t *ops)
{
    if(pdrv < DISKIO_MAX_DRIVES)
        drive_ops[pdrv] = ops;
}

static inline const diskio_ops_t *ops_for (BYTE pdrv)
{
    return pdrv < DISKIO_MAX_DRIVES ? drive_ops[pdrv] : NULL;
}

/*-----------------------------------------------------------------------*/
/* FatFs disk_* entry points - forward to the registered backend         */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize (BYTE pdrv)
{
    const diskio_ops_t *ops = ops_for(pdrv);

    if(ops == NULL || ops->initialize == NULL)
        return STA_NOINIT;

    return ops->initialize();
}

DSTATUS disk_status (BYTE pdrv)
{
    const diskio_ops_t *ops = ops_for(pdrv);

    if(ops == NULL || ops->status == NULL)
        return STA_NOINIT;

    return ops->status();
}

DRESULT disk_read (BYTE pdrv, BYTE *buff, DWORD sector, BYTE count)
{
    const diskio_ops_t *ops = ops_for(pdrv);

    if(ops == NULL || ops->read == NULL)
        return RES_NOTRDY;

    return ops->read(buff, sector, count);
}

#if FF_FS_READONLY == 0
DRESULT disk_write (BYTE pdrv, const BYTE *buff, DWORD sector, BYTE count)
{
    const diskio_ops_t *ops = ops_for(pdrv);

    if(ops == NULL || ops->write == NULL)
        return RES_NOTRDY;

    return ops->write(buff, sector, count);
}
#endif

DRESULT disk_ioctl (BYTE pdrv, BYTE cmd, void *buff)
{
    const diskio_ops_t *ops = ops_for(pdrv);

    if(ops == NULL || ops->ioctl == NULL)
        return RES_PARERR;

    return ops->ioctl(cmd, buff);
}

/*-----------------------------------------------------------------------*/
/* Device Timer Interrupt Procedure (called every 10 ms)                 */
/*-----------------------------------------------------------------------*/

void disk_timerproc (void)
{
    for(BYTE pdrv = 0; pdrv < DISKIO_MAX_DRIVES; pdrv++) {
        const diskio_ops_t *ops = drive_ops[pdrv];
        if(ops && ops->timerproc)
            ops->timerproc();
    }
}

/*---------------------------------------------------------*/
/* User Provided Timer Function for FatFs module           */
/*---------------------------------------------------------*/
/* This is a real time clock service to be called from     */
/* FatFs module. Any valid time must be returned even if   */
/* the system does not support a real time clock.          */

DWORD get_fattime (void)
{
    struct tm time;
    DWORD dt = ((2007UL-1980) << 25) | // Year = 2007
                (6UL << 21) |          // Month = June
                (5UL << 16) |          // Day = 5
                (11U << 11) |          // Hour = 11
                (38U << 5) |           // Min = 38
                (0U >> 1);             // Sec = 0

    if(hal.rtc.get_datetime && hal.rtc.get_datetime(&time))
        dt = ((time.tm_year - 80) << 25) |
             ((time.tm_mon + 1) << 21) |
              (time.tm_mday << 16) |
              (time.tm_hour << 11) |
              (time.tm_min << 5) |
              (time.tm_sec >> 1);

    return dt;
}
