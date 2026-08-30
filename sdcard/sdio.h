/*
  sdio.h - PIO based 4-bit SDIO backend for grblHAL FatFs (RP2040/RP2350)

  Part of grblHAL

  Provides the block device primitives used by fatfs/diskio.c when the board
  is configured for a native SD (SDIO) interface (SDCARD_SDIO == 1).

  Pin configuration comes from the board map:
    SDIO_CLK_PIN, SDIO_CMD_PIN, SDIO_D0_PIN (D0..D3 must be consecutive)
*/

#ifndef _GRBLHAL_SDIO_H_
#define _GRBLHAL_SDIO_H_

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    SDIO_OK = 0,
    SDIO_ERR_INIT,      // card did not initialise
    SDIO_ERR_RESPONSE,  // no / bad command response
    SDIO_ERR_CRC,       // CRC mismatch
    SDIO_ERR_TIMEOUT,   // operation timed out
    SDIO_ERR_PARAM      // bad argument
} sdio_result_t;

// Initialise the SD card over SDIO. Returns SDIO_OK when a card is ready.
sdio_result_t sdio_init (void);

// Read count 512-byte sectors starting at lba into buff.
sdio_result_t sdio_read_sectors (uint8_t *buff, uint32_t lba, uint32_t count);

// Write count 512-byte sectors starting at lba from buff.
sdio_result_t sdio_write_sectors (const uint8_t *buff, uint32_t lba, uint32_t count);

// Total number of 512-byte sectors on the card (0 if unknown / not mounted).
uint32_t sdio_get_sector_count (void);

// True once a card has been successfully initialised.
bool sdio_card_ready (void);

// 10 ms tick used for internal timeouts (call from disk_timerproc).
void sdio_timerproc (void);

// Register this SDIO backend as the FatFs block device for a physical drive.
// (pdrv is a FatFs BYTE drive number.) Call during storage init.
void sdio_register (uint8_t pdrv);

#endif // _GRBLHAL_SDIO_H_
