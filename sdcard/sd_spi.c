/*
  sd_spi.c - SD/MMC card block device over SPI for grblHAL FatFs

  Part of grblHAL

  MMC/SDC (SPI mode) control module, (C)ChaN, 2007. Adapted for RP2040/RP2350
  and moved out of fatfs/diskio.c into a self-registering block-device backend.
  The card protocol logic is unchanged from the original ChaN sample; the file
  now exposes sd_spi_register() which installs a diskio_ops_t so diskio.c can
  forward to it.

  Compiled for SD-over-SPI, i.e. when SDCARD_ENABLE && !SDCARD_SDIO.
*/

#include <stdint.h>
#include <stdbool.h>

#include "driver.h"

#if SDCARD_ENABLE && !SDCARD_SDIO

#include "ff.h"
#include "diskio.h"
#include "../fatfs/diskio_register.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

/* Definitions for MMC/SDC command */
#define CMD0    (0x40+0)    /* GO_IDLE_STATE */
#define CMD1    (0x40+1)    /* SEND_OP_COND */
#define CMD8    (0x40+8)    /* SEND_IF_COND */
#define CMD9    (0x40+9)    /* SEND_CSD */
#define CMD10   (0x40+10)   /* SEND_CID */
#define CMD12   (0x40+12)   /* STOP_TRANSMISSION */
#define CMD16   (0x40+16)   /* SET_BLOCKLEN */
#define CMD17   (0x40+17)   /* READ_SINGLE_BLOCK */
#define CMD18   (0x40+18)   /* READ_MULTIPLE_BLOCK */
#define CMD23   (0x40+23)   /* SET_BLOCK_COUNT */
#define CMD24   (0x40+24)   /* WRITE_BLOCK */
#define CMD25   (0x40+25)   /* WRITE_MULTIPLE_BLOCK */
#define CMD41   (0x40+41)   /* SEND_OP_COND (ACMD) */
#define CMD55   (0x40+55)   /* APP_CMD */
#define CMD58   (0x40+58)   /* READ_OCR */

#define BOOL bool
#define TRUE true
#define FALSE false

#ifndef SDCARD_SPI_FREQ
#define SDCARD_SPI_FREQ 12000000 // 12MHz
#endif
#ifndef SDCARD_USE_DMA
#define SDCARD_USE_DMA 1
#endif

static spi_slave_t dev = {
    .cs_pin = SD_CS_PIN,
    .f_clock = 400000
};

static inline void SELECT (void)   { spi_select(&dev); }
static inline void DESELECT (void) { spi_deselect(&dev); }

/*--------------------------------------------------------------------------
   Module Private Functions
---------------------------------------------------------------------------*/

static volatile DSTATUS Stat = STA_NOINIT;    /* Disk status */
static volatile BYTE Timer1, Timer2;          /* 100Hz decrement timer */
static BYTE CardType;                          /* b0:MMC, b1:SDC, b2:Block addressing */
static BYTE PowerFlag = 0;                     /* indicates if "power" is on */

static void xmit_spi (BYTE dat)
{
    spi_put_byte(dat);
}

static BYTE rcvr_spi (void)
{
    return spi_get_byte();
}

static void rcvr_spi_m (BYTE *dst)
{
    *dst = rcvr_spi();
}

/* Wait for card ready */
static BYTE wait_ready (void)
{
    BYTE res;

    Timer2 = 50;    /* Wait for ready in timeout of 500ms */
    rcvr_spi();
    do
        res = rcvr_spi();
    while ((res != 0xFF) && Timer2);

    return res;
}

/* Send 80 or so clock transitions with CS and DI held high to get the card
   into SPI mode after power up. */
static void send_initial_clock_train (void)
{
    unsigned int i = 10;

    dev.f_clock = 400000;
    SELECT();
    DESELECT();     /* Ensure CS is held high. */

    while(i--)
        xmit_spi(0xFF);
}

/* Power control (initializes the SPI port/pins on first use). */
static void power_on (void)
{
    static bool init = false;

    if(!init) {
        spi_start(&dev);
        init = true;
    }

    dev.f_clock = 400000;
    PowerFlag = 1;
}

static void set_max_speed (void)
{
    dev.f_clock = SDCARD_SPI_FREQ;
}

static void power_off (void)
{
    PowerFlag = 0;
}

static int chk_power (void)   /* Socket power state: 0=off, 1=on */
{
    return PowerFlag;
}

/* Receive a data packet from MMC */
static BOOL rcvr_datablock (BYTE *buff, UINT btr)
{
    BYTE token;

    Timer1 = 100;
    do {                            /* Wait for data packet in timeout of 100ms */
        token = rcvr_spi();
    } while ((token == 0xFF) && Timer1);
    if(token != 0xFE) return FALSE; /* If not valid data token, return with error */

#if SDCARD_USE_DMA
    spi_read((uint8_t *)buff, btr); /* Receive the data block into buffer */
#else
    do {
        rcvr_spi_m(buff++);
        rcvr_spi_m(buff++);
    } while (btr -= 2);
#endif
    rcvr_spi();                     /* Discard CRC */
    rcvr_spi();

    return TRUE;
}

/* Send a data packet to MMC */
#if FF_FS_READONLY == 0
static BOOL xmit_datablock (const BYTE *buff, BYTE token)
{
    BYTE resp;

    if (wait_ready() != 0xFF) return FALSE;

    xmit_spi(token);                     /* Xmit data token */
    if (token != 0xFD) {                 /* Is data token */
    #if SDCARD_USE_DMA
        spi_write((uint8_t *)buff, 512); /* Xmit the 512 byte data block to MMC */
    #else
        BYTE wc = 0;
        do {
            xmit_spi(*buff++);
            xmit_spi(*buff++);
        } while (--wc);
    #endif
        xmit_spi(0xFF);                  /* CRC (Dummy) */
        xmit_spi(0xFF);
        resp = rcvr_spi();               /* Receive data response */
        if ((resp & 0x1F) != 0x05)       /* If not accepted, return with error */
            return FALSE;
    }

    return TRUE;
}
#endif /* _READONLY */

/* Send a command packet to MMC */
static BYTE send_cmd (BYTE cmd, DWORD arg)
{
    BYTE n, res;

    if (wait_ready() != 0xFF) return 0xFF;

    /* Send command packet */
    xmit_spi(cmd);                   /* Command */
    xmit_spi((BYTE)(arg >> 24));     /* Argument[31..24] */
    xmit_spi((BYTE)(arg >> 16));     /* Argument[23..16] */
    xmit_spi((BYTE)(arg >> 8));      /* Argument[15..8] */
    xmit_spi((BYTE)arg);             /* Argument[7..0] */
    n = 0xff;
    if (cmd == CMD0) n = 0x95;       /* CRC for CMD0(0) */
    if (cmd == CMD8) n = 0x87;       /* CRC for CMD8(0x1AA) */
    xmit_spi(n);

    /* Receive command response */
    if (cmd == CMD12) rcvr_spi();    /* Skip a stuff byte when stop reading */
    n = 10;                          /* Wait for a valid response in timeout of 10 attempts */
    do
        res = rcvr_spi();
    while ((res & 0x80) && --n);

    return res;
}

/* Terminate a multi-sector read (CMD12). See original ChaN notes. */
static BYTE send_cmd12 (void)
{
    BYTE n, res = 0xFF, val;

    xmit_spi(CMD12);
    xmit_spi(0);
    xmit_spi(0);
    xmit_spi(0);
    xmit_spi(0);
    xmit_spi(0);

    for(n = 0; n < 10; n++) {
        val = rcvr_spi();
        if(val != 0xFF)
            res = val;
    }

    return res;
}

/*--------------------------------------------------------------------------
   Block device operations
---------------------------------------------------------------------------*/

static DSTATUS sd_spi_initialize (void)
{
    BYTE n, ty, ocr[4];

    if (Stat & STA_NODISK) return Stat;    /* No card in the socket */

    power_on();                            /* Force socket power on */
    send_initial_clock_train();            /* Ensure the card is in SPI mode */

    SELECT();                              /* CS = L */

    ty = 0;
    if (send_cmd(CMD0, 0) == 1) {          /* Enter Idle state */
        Timer1 = 100;                      /* Initialization timeout of 1000 msec */
        if (send_cmd(CMD8, 0x1AA) == 1) {  /* SDC Ver2+ */
            for (n = 0; n < 4; n++) ocr[n] = rcvr_spi();
            if (ocr[2] == 0x01 && ocr[3] == 0xAA) {    /* 2.7-3.6V range */
                do {
                    if (send_cmd(CMD55, 0) <= 1 && send_cmd(CMD41, 1UL << 30) == 0) break;    /* ACMD41 with HCS */
                } while (Timer1);
                if (Timer1 && send_cmd(CMD58, 0) == 0) {    /* Check CCS bit */
                    for (n = 0; n < 4; n++) ocr[n] = rcvr_spi();
                    ty = (ocr[0] & 0x40) ? 6 : 2;
                }
            }
        } else {                           /* SDC Ver1 or MMC */
            ty = (send_cmd(CMD55, 0) <= 1 && send_cmd(CMD41, 0) <= 1) ? 2 : 1;    /* SDC : MMC */
            do {
                if (ty == 2) {
                    if (send_cmd(CMD55, 0) <= 1 && send_cmd(CMD41, 0) == 0) break;    /* ACMD41 */
                } else {
                    if (send_cmd(CMD1, 0) == 0) break;                                /* CMD1 */
                }
            } while (Timer1);
            if (!Timer1 || send_cmd(CMD16, 512) != 0)    /* Select R/W block length */
                ty = 0;
        }
    }

    CardType = ty;
    DESELECT();            /* CS = H */
    rcvr_spi();            /* Idle (Release DO) */

    if (ty) {              /* Initialization succeeded */
        Stat &= ~STA_NOINIT;
        set_max_speed();
    } else {               /* Initialization failed */
        power_off();
    }

    return Stat;
}

static DSTATUS sd_spi_status (void)
{
    return Stat;
}

static DRESULT sd_spi_read (BYTE *buff, DWORD sector, BYTE count)
{
    if (!count) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;

    if (!(CardType & 4)) sector *= 512;    /* Convert to byte address if needed */

    SELECT();

    if (count == 1) {    /* Single block read */
        if ((send_cmd(CMD17, sector) == 0) && rcvr_datablock(buff, 512))
            count = 0;
    } else {             /* Multiple block read */
        if (send_cmd(CMD18, sector) == 0) {
            do {
                if (!rcvr_datablock(buff, 512)) break;
                buff += 512;
            } while (--count);
            send_cmd12();                /* STOP_TRANSMISSION */
        }
    }

    DESELECT();
    rcvr_spi();

    return count ? RES_ERROR : RES_OK;
}

#if FF_FS_READONLY == 0
static DRESULT sd_spi_write (const BYTE *buff, DWORD sector, BYTE count)
{
    if (!count) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;
    if (Stat & STA_PROTECT) return RES_WRPRT;

    if (!(CardType & 4)) sector *= 512;

    SELECT();

    if (count == 1) {    /* Single block write */
        if ((send_cmd(CMD24, sector) == 0) && xmit_datablock(buff, 0xFE))
            count = 0;
    } else {             /* Multiple block write */
        if (CardType & 2) {
            send_cmd(CMD55, 0); send_cmd(CMD23, count);    /* ACMD23 */
        }
        if (send_cmd(CMD25, sector) == 0) {
            do {
                if (!xmit_datablock(buff, 0xFC)) break;
                buff += 512;
            } while (--count);
            if (!xmit_datablock(0, 0xFD))    /* STOP_TRAN token */
                count = 1;
        }
    }

    DESELECT();
    rcvr_spi();

    return count ? RES_ERROR : RES_OK;
}
#endif /* _READONLY */

static DRESULT sd_spi_ioctl (BYTE ctrl, void *buff)
{
    DRESULT res;
    BYTE n, csd[16], *ptr = buff;
    WORD csize;

    res = RES_ERROR;

    if (ctrl == CTRL_POWER) {
        switch (*ptr) {
        case 0:        /* POWER_OFF */
            if (chk_power())
                power_off();
            res = RES_OK;
            break;
        case 1:        /* POWER_ON */
            power_on();
            res = RES_OK;
            break;
        case 2:        /* POWER_GET */
            *(ptr+1) = (BYTE)chk_power();
            res = RES_OK;
            break;
        default:
            res = RES_PARERR;
        }
    } else {
        if (Stat & STA_NOINIT) return RES_NOTRDY;

        SELECT();

        switch (ctrl) {
        case GET_SECTOR_COUNT:    /* Number of sectors on the disk (DWORD) */
            if ((send_cmd(CMD9, 0) == 0) && rcvr_datablock(csd, 16)) {
                if ((csd[0] >> 6) == 1) {    /* SDC ver 2.00 */
                    csize = csd[9] + ((WORD)csd[8] << 8) + 1;
                    *(DWORD*)buff = (DWORD)csize << 10;
                } else {                    /* MMC or SDC ver 1.XX */
                    n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
                    csize = (csd[8] >> 6) + ((WORD)csd[7] << 2) + ((WORD)(csd[6] & 3) << 10) + 1;
                    *(DWORD*)buff = (DWORD)csize << (n - 9);
                }
                res = RES_OK;
            }
            break;

        case GET_SECTOR_SIZE:
            *(WORD*)buff = 512;
            res = RES_OK;
            break;

        case CTRL_SYNC:
            if (wait_ready() == 0xFF)
                res = RES_OK;
            break;

        case MMC_GET_CSD:
            if (send_cmd(CMD9, 0) == 0 && rcvr_datablock(ptr, 16))
                res = RES_OK;
            break;

        case MMC_GET_CID:
            if (send_cmd(CMD10, 0) == 0 && rcvr_datablock(ptr, 16))
                res = RES_OK;
            break;

        case MMC_GET_OCR:
            if (send_cmd(CMD58, 0) == 0) {
                for (n = 0; n < 4; n++)
                    *ptr++ = rcvr_spi();
                res = RES_OK;
            }
            break;

        default:
            res = RES_PARERR;
        }

        DESELECT();
        rcvr_spi();
    }

    return res;
}

/* 100Hz decrement timer, called every 10 ms. */
static void sd_spi_timerproc (void)
{
    BYTE n;

    n = Timer1;
    if (n) Timer1 = --n;
    n = Timer2;
    if (n) Timer2 = --n;
}

static const diskio_ops_t sd_spi_ops = {
    .initialize = sd_spi_initialize,
    .status     = sd_spi_status,
    .read       = sd_spi_read,
#if FF_FS_READONLY == 0
    .write      = sd_spi_write,
#else
    .write      = NULL,
#endif
    .ioctl      = sd_spi_ioctl,
    .timerproc  = sd_spi_timerproc
};

void sd_spi_register (BYTE pdrv)
{
    diskio_register(pdrv, &sd_spi_ops);
}

#endif // SDCARD_ENABLE && !SDCARD_SDIO
