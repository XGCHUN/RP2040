/*
  sd_spi.h - SD/MMC over SPI block device registration

  Part of grblHAL
*/

#ifndef _GRBLHAL_SD_SPI_H_
#define _GRBLHAL_SD_SPI_H_

#include <stdint.h>

// Register the SPI SD backend as the FatFs block device for a physical drive.
// (pdrv is a FatFs BYTE drive number.) Call during storage init.
void sd_spi_register (uint8_t pdrv);

#endif // _GRBLHAL_SD_SPI_H_
