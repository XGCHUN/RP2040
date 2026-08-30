/*
  diskio_register.h - runtime block-device registration for FatFs diskio

  Part of grblHAL

  A low-level block device driver (SD over SPI, SD over SDIO, eMMC, ...)
  fills in a diskio_ops_t and registers it for a physical drive number with
  diskio_register(). fatfs/diskio.c then forwards the FatFs disk_* calls to
  the registered ops, so adding or swapping a backend never requires editing
  diskio.c.

  Registration must happen before the first FatFs f_mount() for that drive
  (typically from the driver's storage init, e.g. sdcard_init()).

  The op signatures mirror the FatFs disk_* prototypes in diskio.h.
*/

#ifndef _DISKIO_REGISTER_H_
#define _DISKIO_REGISTER_H_

#include "ff.h"
#include "diskio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Number of physical drives supported by the forwarding layer.
#ifndef DISKIO_MAX_DRIVES
#define DISKIO_MAX_DRIVES 1
#endif

typedef struct {
    DSTATUS (*initialize)(void);                                // bring up / probe the medium
    DSTATUS (*status)(void);                                    // current STA_* status
    DRESULT (*read)(BYTE *buff, DWORD sector, BYTE count);      // read count sectors
    DRESULT (*write)(const BYTE *buff, DWORD sector, BYTE count); // write count sectors (may be NULL if read-only)
    DRESULT (*ioctl)(BYTE cmd, void *buff);                     // control / query
    void    (*timerproc)(void);                                 // 10 ms tick (may be NULL)
} diskio_ops_t;

// Register (or replace) the block-device ops for a physical drive.
// ops must remain valid for the lifetime of the program (typically static).
void diskio_register (BYTE pdrv, const diskio_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif // _DISKIO_REGISTER_H_
