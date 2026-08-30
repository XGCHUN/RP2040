/*
  sdio.c - PIO based 4-bit SDIO backend for grblHAL FatFs (RP2040/RP2350)

  Part of grblHAL

  The low-level SDIO bus engine (PIO programs in sdio.pio, the CRC7 table, the
  4-bit CRC16 algorithm and the command/response packet format) is ported from
  carlk3's no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
    https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
  whose SDIO driver is derived from ZuluSCSI-firmware (both permissively
  licensed). The card enumeration and the grblHAL block API glue below are
  written for this project.

  Only compiled when SDCARD_ENABLE && SDCARD_SDIO.

  Bus layout (from the board map):
    SDIO_CLK_PIN, SDIO_CMD_PIN, SDIO_D0_PIN (D0..D3 consecutive).
  The PIO requires CLK = D0 - 1 (see SDIO_CLK_PIN_D0_OFFSET in sdio.pio).
*/

#include "driver.h"

#if SDCARD_ENABLE && SDCARD_SDIO

#include <string.h>

#include "sdio.h"
#include "../fatfs/diskio_register.h"

#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"

#include "sdio.pio.h"

#ifndef SDIO_CLK_PIN
#error "SDIO_CLK_PIN must be defined in the board map"
#endif
#ifndef SDIO_CMD_PIN
#error "SDIO_CMD_PIN must be defined in the board map"
#endif
#ifndef SDIO_D0_PIN
#error "SDIO_D0_PIN must be defined in the board map (D0..D3 consecutive)"
#endif

// The PIO data programs derive the CLK position from D0 (CLK = D0 - 1).
#if (SDIO_CLK_PIN + 1) != SDIO_D0_PIN
#error "SDIO pin layout requires SDIO_CLK_PIN == SDIO_D0_PIN - 1"
#endif

// D1..D3 are optional in the board map (the driver only needs the D0 base and
// uses D0+1..D0+3). When a board defines them explicitly, verify they form the
// required 4 consecutive GPIOs.
#if defined(SDIO_D1_PIN) && (SDIO_D1_PIN != (SDIO_D0_PIN + 1))
#error "SDIO_D1_PIN must equal SDIO_D0_PIN + 1 (data lines must be consecutive)"
#endif
#if defined(SDIO_D2_PIN) && (SDIO_D2_PIN != (SDIO_D0_PIN + 2))
#error "SDIO_D2_PIN must equal SDIO_D0_PIN + 2 (data lines must be consecutive)"
#endif
#if defined(SDIO_D3_PIN) && (SDIO_D3_PIN != (SDIO_D0_PIN + 3))
#error "SDIO_D3_PIN must equal SDIO_D0_PIN + 3 (data lines must be consecutive)"
#endif

// --- SD commands ---
#define CMD0    0   // GO_IDLE_STATE
#define CMD2    2   // ALL_SEND_CID
#define CMD3    3   // SEND_RELATIVE_ADDR
#define CMD7    7   // SELECT_CARD
#define CMD8    8   // SEND_IF_COND
#define CMD9    9   // SEND_CSD
#define CMD12   12  // STOP_TRANSMISSION
#define CMD13   13  // SEND_STATUS
#define CMD16   16  // SET_BLOCKLEN
#define CMD17   17  // READ_SINGLE_BLOCK
#define CMD24   24  // WRITE_BLOCK
#define CMD55   55  // APP_CMD
#define ACMD6   6   // SET_BUS_WIDTH (after CMD55)
#define ACMD41  41  // SD_SEND_OP_COND (after CMD55)

#define OCR_HCS         (1UL << 30)
#define OCR_VOLTAGE     0x00FF8000UL
#define OCR_BUSY        (1UL << 31)
#define R1_READY_FOR_DATA (1UL << 8)

#define SDIO_BLOCK_SIZE     512u
#define SDIO_WORDS_PER_BLOCK (SDIO_BLOCK_SIZE / 4u)   // 128

typedef enum {
    CARD_NONE = 0,
    CARD_SD1,
    CARD_SD2,
    CARD_SDHC
} card_type_t;

static PIO   sdio_pio  = NULL;
static uint  cmd_sm    = 0;   // command / clock state machine
static uint  data_sm   = 1;   // data state machine
static int   dma_ch    = -1;

static uint  off_cmd_clk = 0;
static uint  off_data_rx = 0;
static uint  off_data_tx = 0;
static pio_sm_config cfg_data_rx;
static pio_sm_config cfg_data_tx;

static float clk_div_slow = 0.0f;
static float clk_div_fast = 0.0f;

static card_type_t card_type = CARD_NONE;
static uint32_t     card_rca  = 0;
static uint32_t     card_sectors = 0;
static bool         card_ready = false;

static volatile uint32_t tick_10ms = 0;

// Scratch buffer for the write path: start token + data + CRC + end token.
static uint32_t tx_block_buf[1 + SDIO_WORDS_PER_BLOCK + 2 + 1];

void sdio_timerproc (void)
{
    tick_10ms++;
}

static uint32_t now_ms (void)
{
    return to_ms_since_boot(get_absolute_time());
}

/*******************************************************
 * Checksum algorithms (from carlk3 / ZuluSCSI)
 *******************************************************/

// CRC-7 table for SDIO command packets.
static const uint8_t crc7_table[256] = {
    0x00, 0x12, 0x24, 0x36, 0x48, 0x5a, 0x6c, 0x7e, 0x90, 0x82, 0xb4, 0xa6, 0xd8, 0xca, 0xfc, 0xee,
    0x32, 0x20, 0x16, 0x04, 0x7a, 0x68, 0x5e, 0x4c, 0xa2, 0xb0, 0x86, 0x94, 0xea, 0xf8, 0xce, 0xdc,
    0x64, 0x76, 0x40, 0x52, 0x2c, 0x3e, 0x08, 0x1a, 0xf4, 0xe6, 0xd0, 0xc2, 0xbc, 0xae, 0x98, 0x8a,
    0x56, 0x44, 0x72, 0x60, 0x1e, 0x0c, 0x3a, 0x28, 0xc6, 0xd4, 0xe2, 0xf0, 0x8e, 0x9c, 0xaa, 0xb8,
    0xc8, 0xda, 0xec, 0xfe, 0x80, 0x92, 0xa4, 0xb6, 0x58, 0x4a, 0x7c, 0x6e, 0x10, 0x02, 0x34, 0x26,
    0xfa, 0xe8, 0xde, 0xcc, 0xb2, 0xa0, 0x96, 0x84, 0x6a, 0x78, 0x4e, 0x5c, 0x22, 0x30, 0x06, 0x14,
    0xac, 0xbe, 0x88, 0x9a, 0xe4, 0xf6, 0xc0, 0xd2, 0x3c, 0x2e, 0x18, 0x0a, 0x74, 0x66, 0x50, 0x42,
    0x9e, 0x8c, 0xba, 0xa8, 0xd6, 0xc4, 0xf2, 0xe0, 0x0e, 0x1c, 0x2a, 0x38, 0x46, 0x54, 0x62, 0x70,
    0x82, 0x90, 0xa6, 0xb4, 0xca, 0xd8, 0xee, 0xfc, 0x12, 0x00, 0x36, 0x24, 0x5a, 0x48, 0x7e, 0x6c,
    0xb0, 0xa2, 0x94, 0x86, 0xf8, 0xea, 0xdc, 0xce, 0x20, 0x32, 0x04, 0x16, 0x68, 0x7a, 0x4c, 0x5e,
    0xe6, 0xf4, 0xc2, 0xd0, 0xae, 0xbc, 0x8a, 0x98, 0x76, 0x64, 0x52, 0x40, 0x3e, 0x2c, 0x1a, 0x08,
    0xd4, 0xc6, 0xf0, 0xe2, 0x9c, 0x8e, 0xb8, 0xaa, 0x44, 0x56, 0x60, 0x72, 0x0c, 0x1e, 0x28, 0x3a,
    0x4a, 0x58, 0x6e, 0x7c, 0x02, 0x10, 0x26, 0x34, 0xda, 0xc8, 0xfe, 0xec, 0x92, 0x80, 0xb6, 0xa4,
    0x78, 0x6a, 0x5c, 0x4e, 0x30, 0x22, 0x14, 0x06, 0xe8, 0xfa, 0xcc, 0xde, 0xa0, 0xb2, 0x84, 0x96,
    0x2e, 0x3c, 0x0a, 0x18, 0x66, 0x74, 0x42, 0x50, 0xbe, 0xac, 0x9a, 0x88, 0xf6, 0xe4, 0xd2, 0xc0,
    0x1c, 0x0e, 0x38, 0x2a, 0x54, 0x46, 0x70, 0x62, 0x8c, 0x9e, 0xa8, 0xba, 0xc4, 0xd6, 0xe0, 0xf2
};

// Parallel 4-bit-line CRC16 (CCITT) used for SDIO data blocks. Produces the
// 4 x 16 = 64 bit checksum, one 16-bit CRC per data line.
static uint64_t sdio_crc16_4bit_checksum (const uint32_t *data, uint32_t num_words)
{
    uint64_t crc = 0;
    const uint32_t *end = data + num_words;

    while(data < end) {
        for(int unroll = 0; unroll < 4; unroll++) {
            uint32_t data_in = __builtin_bswap32(*data++);
            uint32_t data_out = crc >> 32;
            crc <<= 32;
            data_out ^= (data_out >> 16);
            data_out ^= (data_in >> 16);
            uint64_t xorred = data_out ^ data_in;
            crc ^= xorred;
            crc ^= xorred << (5 * 4);
            crc ^= xorred << (12 * 4);
        }
    }

    return crc;
}

/*******************************************************
 * Command execution (packet format from carlk3 / ZuluSCSI)
 *******************************************************/

static void sdio_send_command (uint8_t command, uint32_t arg, uint8_t response_bits)
{
    uint32_t word0 =
        (47u << 24) |               // number of bits in command minus one
        (1u  << 22) |               // transfer direction host -> card
        ((uint32_t)command << 16) |
        (((arg >> 24) & 0xFF) << 8) |
        (((arg >> 16) & 0xFF) << 0);
    uint32_t word1 =
        (((arg >> 8) & 0xFF) << 24) |
        (((arg >> 0) & 0xFF) << 16) |
        (1u << 8);                  // end bit

    if(response_bits)
        word1 |= ((response_bits - 1u) << 0);

    uint8_t crc = 0;
    crc = crc7_table[crc ^ ((word0 >> 16) & 0xFF)];
    crc = crc7_table[crc ^ ((word0 >>  8) & 0xFF)];
    crc = crc7_table[crc ^ ((word0 >>  0) & 0xFF)];
    crc = crc7_table[crc ^ ((word1 >> 24) & 0xFF)];
    crc = crc7_table[crc ^ ((word1 >> 16) & 0xFF)];
    word1 |= crc << 8;

    pio_sm_clear_fifos(sdio_pio, cmd_sm);
    pio_sm_put(sdio_pio, cmd_sm, word0);
    pio_sm_put(sdio_pio, cmd_sm, word1);
}

// R1/R6/R7: 48-bit response. If response is NULL, no reply is awaited.
static sdio_result_t sdio_command_R1 (uint8_t command, uint32_t arg, uint32_t *response)
{
    sdio_send_command(command, arg, response ? 48 : 0);

    uint32_t wait_words = response ? 2 : 1;
    uint32_t deadline = now_ms() + 200;
    while(pio_sm_get_rx_fifo_level(sdio_pio, cmd_sm) < wait_words) {
        if(now_ms() > deadline) {
            pio_sm_clear_fifos(sdio_pio, cmd_sm);
            pio_sm_exec(sdio_pio, cmd_sm, pio_encode_jmp(off_cmd_clk));
            return SDIO_ERR_TIMEOUT;
        }
    }

    if(response) {
        uint32_t resp0 = pio_sm_get(sdio_pio, cmd_sm);
        uint32_t resp1 = pio_sm_get(sdio_pio, cmd_sm);
        uint8_t crc = 0;
        crc = crc7_table[crc ^ ((resp0 >> 24) & 0xFF)];
        crc = crc7_table[crc ^ ((resp0 >> 16) & 0xFF)];
        crc = crc7_table[crc ^ ((resp0 >>  8) & 0xFF)];
        crc = crc7_table[crc ^ ((resp0 >>  0) & 0xFF)];
        crc = crc7_table[crc ^ ((resp1 >>  8) & 0xFF)];
        uint8_t actual_crc = ((resp1 >> 0) & 0xFE);
        if(crc != actual_crc)
            return SDIO_ERR_CRC;
        uint8_t response_cmd = ((resp0 >> 24) & 0xFF);
        if(response_cmd != command && command != 41)
            return SDIO_ERR_RESPONSE;
        *response = ((resp0 & 0xFFFFFF) << 8) | ((resp1 >> 8) & 0xFF);
    } else
        pio_sm_get(sdio_pio, cmd_sm);   // discard dummy marker

    return SDIO_OK;
}

// R3: 48-bit response without CRC (OCR).
static sdio_result_t sdio_command_R3 (uint8_t command, uint32_t arg, uint32_t *response)
{
    sdio_send_command(command, arg, 48);

    uint32_t deadline = now_ms() + 200;
    while(pio_sm_get_rx_fifo_level(sdio_pio, cmd_sm) < 2) {
        if(now_ms() > deadline) {
            pio_sm_clear_fifos(sdio_pio, cmd_sm);
            pio_sm_exec(sdio_pio, cmd_sm, pio_encode_jmp(off_cmd_clk));
            return SDIO_ERR_TIMEOUT;
        }
    }

    uint32_t resp0 = pio_sm_get(sdio_pio, cmd_sm);
    uint32_t resp1 = pio_sm_get(sdio_pio, cmd_sm);
    *response = ((resp0 & 0xFFFFFF) << 8) | ((resp1 >> 8) & 0xFF);

    return SDIO_OK;
}

// R2: 136-bit response (CID/CSD). response buffer must hold 16 bytes.
static sdio_result_t sdio_command_R2 (uint8_t command, uint32_t arg, uint8_t *response)
{
    uint32_t buf[5];

    pio_sm_clear_fifos(sdio_pio, cmd_sm);

    dma_channel_config c = dma_channel_get_default_config(dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(sdio_pio, cmd_sm, false));
    dma_channel_configure(dma_ch, &c, buf, &sdio_pio->rxf[cmd_sm], 5, true);

    sdio_send_command(command, arg, 136);

    uint32_t deadline = now_ms() + 200;
    while(dma_channel_is_busy(dma_ch)) {
        if(now_ms() > deadline) {
            dma_channel_abort(dma_ch);
            pio_sm_clear_fifos(sdio_pio, cmd_sm);
            pio_sm_exec(sdio_pio, cmd_sm, pio_encode_jmp(off_cmd_clk));
            return SDIO_ERR_TIMEOUT;
        }
    }

    if(response) {
        // The R2 response is 136 bits: an 8-bit prefix (start/transmit/reserved)
        // followed by the 128-bit CID/CSD payload. The PIO pushed it MSB-first
        // as 5 words (word4 holds the final 8 bits in its low byte). Rebuild the
        // 128-bit payload by dropping the top 8 bits, i.e. shift the whole
        // stream left by one byte.
        //   payload[127:96] = (buf0 << 8) | (buf1 >> 24), etc.
        uint32_t p[4];
        p[0] = (buf[0] << 8) | (buf[1] >> 24);
        p[1] = (buf[1] << 8) | (buf[2] >> 24);
        p[2] = (buf[2] << 8) | (buf[3] >> 24);
        p[3] = (buf[3] << 8) | (buf[4] & 0xFF);
        for(int i = 0; i < 16; i++)
            response[i] = (uint8_t)(p[i / 4] >> (24 - 8 * (i & 3)));
        // NOTE: capacity derived from this (card_sectors) is best-effort and
        // not hardware-verified; it does not affect mounting (FatFs reads the
        // real size from the BPB), only $ status / mkfs.
    }

    return SDIO_OK;
}

/*******************************************************
 * Block data transfer (4-bit, synchronous single DMA)
 *******************************************************/

static sdio_result_t sdio_read_block (uint8_t *buff)
{
    // 512 data bytes (1024 nibbles) + 8 CRC bytes (16 nibbles).
    // We DMA the data words; the CRC nibbles are read then discarded.
    uint32_t nibbles = SDIO_BLOCK_SIZE * 2u + 16u;

    // Configure the data SM for receive.
    pio_sm_set_enabled(sdio_pio, data_sm, false);
    pio_sm_clear_fifos(sdio_pio, data_sm);
    pio_sm_init(sdio_pio, data_sm, off_data_rx, &cfg_data_rx);
    pio_sm_set_consecutive_pindirs(sdio_pio, data_sm, SDIO_D0_PIN, 4, false);

    // Y = nibbles to receive - 1.
    pio_sm_put(sdio_pio, data_sm, nibbles - 1u);
    pio_sm_exec(sdio_pio, data_sm, pio_encode_out(pio_y, 32));

    // DMA the incoming words (data + trailing CRC words) into a temp region.
    // We route the 128 data words to buff and 4 extra words (16 CRC nibbles ->
    // 2 words per pair of lines, 4 words total) to a scratch.
    uint32_t scratch[4];
    // First DMA: 128 data words with byte swap (SDIO is big-endian on the bus).
    dma_channel_config c = dma_channel_get_default_config(dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(sdio_pio, data_sm, false));
    channel_config_set_bswap(&c, true);
    dma_channel_configure(dma_ch, &c, buff, &sdio_pio->rxf[data_sm], SDIO_WORDS_PER_BLOCK, true);

    pio_sm_set_enabled(sdio_pio, data_sm, true);

    uint32_t deadline = now_ms() + 500;
    while(dma_channel_is_busy(dma_ch)) {
        if(now_ms() > deadline) {
            dma_channel_abort(dma_ch);
            pio_sm_set_enabled(sdio_pio, data_sm, false);
            return SDIO_ERR_TIMEOUT;
        }
    }

    // Drain the CRC words that follow (2 words = 16 nibbles).
    deadline = now_ms() + 50;
    for(int i = 0; i < 2; i++) {
        while(pio_sm_is_rx_fifo_empty(sdio_pio, data_sm)) {
            if(now_ms() > deadline)
                break;
        }
        scratch[i] = pio_sm_get(sdio_pio, data_sm);
    }
    (void)scratch;

    pio_sm_set_enabled(sdio_pio, data_sm, false);

    // Verify CRC over the received data.
    uint64_t calc = sdio_crc16_4bit_checksum((const uint32_t *)buff, SDIO_WORDS_PER_BLOCK);
    uint32_t top = __builtin_bswap32(scratch[0]);
    uint32_t bottom = __builtin_bswap32(scratch[1]);
    uint64_t expected = ((uint64_t)top << 32) | bottom;
    if(calc != expected)
        return SDIO_ERR_CRC;

    return SDIO_OK;
}

static sdio_result_t sdio_write_block (const uint8_t *buff)
{
    // Build the transmit stream: start token, 128 data words (byte swapped so
    // the bus sees big-endian), 2 CRC words, end token.
    uint64_t crc = sdio_crc16_4bit_checksum((const uint32_t *)buff, SDIO_WORDS_PER_BLOCK);

    tx_block_buf[0] = 0xFFFFFFF0u;   // start token (D0..D3 low on last nibble)
    for(uint32_t i = 0; i < SDIO_WORDS_PER_BLOCK; i++)
        tx_block_buf[1 + i] = __builtin_bswap32(((const uint32_t *)buff)[i]);
    tx_block_buf[1 + SDIO_WORDS_PER_BLOCK + 0] = __builtin_bswap32((uint32_t)(crc >> 32));
    tx_block_buf[1 + SDIO_WORDS_PER_BLOCK + 1] = __builtin_bswap32((uint32_t)crc);
    tx_block_buf[1 + SDIO_WORDS_PER_BLOCK + 2] = 0xFFFFFFFFu;   // end token

    uint32_t total_words = 1 + SDIO_WORDS_PER_BLOCK + 2 + 1;
    uint32_t nibbles = total_words * 8u;   // 132 words * 8 nibbles

    pio_sm_set_enabled(sdio_pio, data_sm, false);
    pio_sm_clear_fifos(sdio_pio, data_sm);
    pio_sm_init(sdio_pio, data_sm, off_data_tx, &cfg_data_tx);

    // X = nibbles to send - 1, Y = response bits - 1 (31).
    pio_sm_put(sdio_pio, data_sm, nibbles - 1u);
    pio_sm_exec(sdio_pio, data_sm, pio_encode_out(pio_x, 32));
    pio_sm_put(sdio_pio, data_sm, 31u);
    pio_sm_exec(sdio_pio, data_sm, pio_encode_out(pio_y, 32));

    // Data pins to output, high.
    pio_sm_exec(sdio_pio, data_sm, pio_encode_set(pio_pins, 15));
    pio_sm_exec(sdio_pio, data_sm, pio_encode_set(pio_pindirs, 15));

    // DMA the stream to the PIO TX FIFO.
    dma_channel_config c = dma_channel_get_default_config(dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(sdio_pio, data_sm, true));
    dma_channel_configure(dma_ch, &c, &sdio_pio->txf[data_sm], tx_block_buf, total_words, true);

    pio_sm_set_enabled(sdio_pio, data_sm, true);

    uint32_t deadline = now_ms() + 500;
    while(dma_channel_is_busy(dma_ch)) {
        if(now_ms() > deadline) {
            dma_channel_abort(dma_ch);
            pio_sm_set_enabled(sdio_pio, data_sm, false);
            return SDIO_ERR_TIMEOUT;
        }
    }

    // Read the D0 CRC-status token pushed by the PIO program.
    deadline = now_ms() + 500;
    while(pio_sm_is_rx_fifo_empty(sdio_pio, data_sm)) {
        if(now_ms() > deadline) {
            pio_sm_set_enabled(sdio_pio, data_sm, false);
            return SDIO_ERR_TIMEOUT;
        }
    }
    uint32_t token = pio_sm_get(sdio_pio, data_sm);
    pio_sm_set_enabled(sdio_pio, data_sm, false);

    // The CRC status is the 5 bits after the start bit: 010 = accepted.
    // Shift until the start (0) bit reaches the expected position.
    uint32_t status = token;
    while(status & 1)                 // find the framing 0 start bit
        status >>= 1;
    uint32_t crc_status = (status >> 1) & 0x7;
    if(crc_status != 0x2)
        return SDIO_ERR_CRC;

    return SDIO_OK;
}

/*******************************************************
 * Resource allocation
 *******************************************************/

static bool claim_resources (void)
{
    // Prefer PIO2 on RP2350B (step pulse uses PIO0/PIO1, stepper timer PIO1,
    // xy2_100 uses one SM of PIO2). Two adjacent SMs and one DMA channel are
    // needed. Fall back to any PIO with two free SMs.
    PIO order[] = {
#ifdef PICO_RP2350B
        pio2,
#endif
        pio1,
        pio0
    };

    for(uint i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        int a = pio_claim_unused_sm(order[i], false);
        if(a < 0)
            continue;
        int b = pio_claim_unused_sm(order[i], false);
        if(b < 0) {
            pio_sm_unclaim(order[i], (uint)a);
            continue;
        }
        sdio_pio = order[i];
        cmd_sm = (uint)a;
        data_sm = (uint)b;
        break;
    }

    if(sdio_pio == NULL)
        return false;

    if((dma_ch = dma_claim_unused_channel(false)) < 0)
        return false;

    return true;
}

/*******************************************************
 * PIO configuration
 *******************************************************/

static bool programs_loaded = false;

static bool pio_setup (float clk_div)
{
    pio_sm_set_enabled(sdio_pio, cmd_sm, false);
    pio_sm_set_enabled(sdio_pio, data_sm, false);

    // Load the three PIO programs into the instruction memory exactly once.
    // pio_setup() may run again on an init retry, and this PIO can be shared
    // with other users (e.g. xy2_100 on PIO2), so we must NOT clear the
    // instruction memory or re-add programs - that would exhaust program space
    // and clobber the other user. Cache the offsets and reuse them.
    if(!programs_loaded) {
        if(!pio_can_add_program(sdio_pio, &sdio_cmd_clk_program) ||
           !pio_can_add_program(sdio_pio, &sdio_data_rx_program) ||
           !pio_can_add_program(sdio_pio, &sdio_data_tx_program))
            return false;
        off_cmd_clk = pio_add_program(sdio_pio, &sdio_cmd_clk_program);
        off_data_rx = pio_add_program(sdio_pio, &sdio_data_rx_program);
        off_data_tx = pio_add_program(sdio_pio, &sdio_data_tx_program);
        programs_loaded = true;
    }

    // --- Command / clock state machine ---
    pio_sm_config cfg = sdio_cmd_clk_program_get_default_config(off_cmd_clk);
    sm_config_set_out_pins(&cfg, SDIO_CMD_PIN, 1);
    sm_config_set_set_pins(&cfg, SDIO_CMD_PIN, 1);
    sm_config_set_in_pins(&cfg, SDIO_CMD_PIN);
    sm_config_set_jmp_pin(&cfg, SDIO_CMD_PIN);
    sm_config_set_sideset_pins(&cfg, SDIO_CLK_PIN);
    sm_config_set_out_shift(&cfg, false, true, 32);  // MSB first, autopull
    sm_config_set_in_shift(&cfg, false, true, 32);   // MSB first, autopush
    // EXECCTRL STATUS = TX level < 2.
    sm_config_set_clkdiv(&cfg, clk_div);

    pio_gpio_init(sdio_pio, SDIO_CLK_PIN);
    pio_gpio_init(sdio_pio, SDIO_CMD_PIN);
    pio_sm_set_consecutive_pindirs(sdio_pio, cmd_sm, SDIO_CLK_PIN, 1, true);
    pio_sm_set_consecutive_pindirs(sdio_pio, cmd_sm, SDIO_CMD_PIN, 1, true);
    pio_sm_init(sdio_pio, cmd_sm, off_cmd_clk, &cfg);
    // Set STATUS to compare against TX FIFO < 2.
    sdio_pio->sm[cmd_sm].execctrl =
        (sdio_pio->sm[cmd_sm].execctrl & ~(PIO_SM0_EXECCTRL_STATUS_SEL_BITS | PIO_SM0_EXECCTRL_STATUS_N_BITS)) |
        (0u << PIO_SM0_EXECCTRL_STATUS_SEL_LSB) |   // 0 = TX FIFO level
        (2u << PIO_SM0_EXECCTRL_STATUS_N_LSB);      // < 2
    pio_sm_set_enabled(sdio_pio, cmd_sm, true);     // continuous clock

    // --- Data state machine configs (applied per transfer) ---
    for(uint i = 0; i < 4; i++)
        pio_gpio_init(sdio_pio, SDIO_D0_PIN + i);

    cfg_data_rx = sdio_data_rx_program_get_default_config(off_data_rx);
    sm_config_set_in_pins(&cfg_data_rx, SDIO_D0_PIN);
    sm_config_set_in_shift(&cfg_data_rx, false, true, 32);  // MSB first, autopush
    sm_config_set_clkdiv(&cfg_data_rx, clk_div);

    cfg_data_tx = sdio_data_tx_program_get_default_config(off_data_tx);
    sm_config_set_out_pins(&cfg_data_tx, SDIO_D0_PIN, 4);
    sm_config_set_set_pins(&cfg_data_tx, SDIO_D0_PIN, 4);
    sm_config_set_in_pins(&cfg_data_tx, SDIO_D0_PIN);
    sm_config_set_out_shift(&cfg_data_tx, false, true, 32); // MSB first, autopull
    sm_config_set_in_shift(&cfg_data_tx, false, true, 32);
    sm_config_set_clkdiv(&cfg_data_tx, clk_div);

    return true;
}

/*******************************************************
 * Card enumeration
 *******************************************************/

sdio_result_t sdio_init (void)
{
    uint32_t resp;
    uint8_t r2[16];

    card_ready = false;
    card_type = CARD_NONE;
    card_rca = 0;
    card_sectors = 0;

    uint32_t sys = clock_get_hz(clk_sys);
    // The PIO uses CLKDIV=4 PIO cycles per SD clock. slow ~400 kHz, fast ~ up
    // to 25 MHz default speed. clkdiv here is the extra SM divider on top.
    clk_div_slow = (float)sys / (4.0f * 400000.0f);
    clk_div_fast = (float)sys / (4.0f * 25000000.0f);
    if(clk_div_fast < 1.0f)
        clk_div_fast = 1.0f;

    if(sdio_pio == NULL) {
        if(!claim_resources())
            return SDIO_ERR_INIT;
    }

    gpio_pull_up(SDIO_CMD_PIN);
    for(uint i = 0; i < 4; i++)
        gpio_pull_up(SDIO_D0_PIN + i);

    if(!pio_setup(clk_div_slow))
        return SDIO_ERR_INIT;

    // Give the card 74+ clocks (the clock SM runs continuously).
    sleep_ms(2);

    // CMD0: go idle.
    sdio_send_command(CMD0, 0, 0);
    sleep_ms(2);
    pio_sm_clear_fifos(sdio_pio, cmd_sm);

    // CMD8: voltage check.
    bool v2 = false;
    if(sdio_command_R1(CMD8, 0x1AA, &resp) == SDIO_OK)
        v2 = ((resp & 0xFFF) == 0x1AA);

    // ACMD41 until the card is ready.
    uint32_t arg = OCR_VOLTAGE | (v2 ? OCR_HCS : 0);
    uint32_t deadline = now_ms() + 1000;
    do {
        if(sdio_command_R1(CMD55, 0, &resp) != SDIO_OK)
            return SDIO_ERR_RESPONSE;
        if(sdio_command_R3(ACMD41, arg, &resp) != SDIO_OK)
            return SDIO_ERR_RESPONSE;
        if(now_ms() > deadline)
            return SDIO_ERR_TIMEOUT;
    } while(!(resp & OCR_BUSY));

    card_type = (resp & OCR_HCS) ? CARD_SDHC : (v2 ? CARD_SD2 : CARD_SD1);

    // CMD2: CID (discarded).
    if(sdio_command_R2(CMD2, 0, r2) != SDIO_OK)
        return SDIO_ERR_RESPONSE;

    // CMD3: relative card address.
    if(sdio_command_R1(CMD3, 0, &resp) != SDIO_OK)
        return SDIO_ERR_RESPONSE;
    card_rca = resp & 0xFFFF0000u;

    // CMD9: CSD -> capacity.
    if(sdio_command_R2(CMD9, card_rca, r2) == SDIO_OK) {
        if((r2[0] >> 6) == 1) {   // CSD v2 (SDHC/SDXC)
            uint32_t csize = ((uint32_t)(r2[7] & 0x3F) << 16) | ((uint32_t)r2[8] << 8) | r2[9];
            card_sectors = (csize + 1) * 1024u;
        } else {                  // CSD v1
            uint32_t read_bl_len = r2[5] & 0x0F;
            uint32_t csize = ((uint32_t)(r2[6] & 0x03) << 10) | ((uint32_t)r2[7] << 2) | (r2[8] >> 6);
            uint32_t csize_mult = ((r2[9] & 0x03) << 1) | (r2[10] >> 7);
            uint32_t blocknr = (csize + 1) * (1u << (csize_mult + 2));
            uint32_t block_len = 1u << read_bl_len;
            card_sectors = (uint32_t)(((uint64_t)blocknr * block_len) / SDIO_BLOCK_SIZE);
        }
    }

    // CMD7: select card.
    if(sdio_command_R1(CMD7, card_rca, &resp) != SDIO_OK)
        return SDIO_ERR_RESPONSE;

    // ACMD6: 4-bit bus.
    if(sdio_command_R1(CMD55, card_rca, &resp) != SDIO_OK)
        return SDIO_ERR_RESPONSE;
    if(sdio_command_R1(ACMD6, 0x2, &resp) != SDIO_OK)
        return SDIO_ERR_RESPONSE;

    // CMD16: block length 512.
    sdio_command_R1(CMD16, SDIO_BLOCK_SIZE, &resp);

    // Switch to full speed for data transfers.
    pio_sm_set_clkdiv(sdio_pio, cmd_sm, clk_div_fast);
    pio_sm_clkdiv_restart(sdio_pio, cmd_sm);
    sm_config_set_clkdiv(&cfg_data_rx, clk_div_fast);
    sm_config_set_clkdiv(&cfg_data_tx, clk_div_fast);

    card_ready = true;

    return SDIO_OK;
}

/*******************************************************
 * Sector read / write (grblHAL block API)
 *******************************************************/

static uint32_t lba_to_addr (uint32_t lba)
{
    return (card_type == CARD_SDHC) ? lba : lba * SDIO_BLOCK_SIZE;
}

sdio_result_t sdio_read_sectors (uint8_t *buff, uint32_t lba, uint32_t count)
{
    uint32_t resp;

    if(!card_ready)
        return SDIO_ERR_INIT;
    if(count == 0)
        return SDIO_ERR_PARAM;

    for(uint32_t i = 0; i < count; i++) {
        if(sdio_command_R1(CMD17, lba_to_addr(lba + i), &resp) != SDIO_OK)
            return SDIO_ERR_RESPONSE;
        sdio_result_t r = sdio_read_block(buff + i * SDIO_BLOCK_SIZE);
        if(r != SDIO_OK)
            return r;
    }

    return SDIO_OK;
}

sdio_result_t sdio_write_sectors (const uint8_t *buff, uint32_t lba, uint32_t count)
{
    uint32_t resp;

    if(!card_ready)
        return SDIO_ERR_INIT;
    if(count == 0)
        return SDIO_ERR_PARAM;

    for(uint32_t i = 0; i < count; i++) {
        if(sdio_command_R1(CMD24, lba_to_addr(lba + i), &resp) != SDIO_OK)
            return SDIO_ERR_RESPONSE;
        sdio_result_t r = sdio_write_block(buff + i * SDIO_BLOCK_SIZE);
        if(r != SDIO_OK)
            return r;
        // Wait for the card to finish programming.
        uint32_t deadline = now_ms() + 500;
        do {
            if(sdio_command_R1(CMD13, card_rca, &resp) != SDIO_OK)
                return SDIO_ERR_RESPONSE;
            if(now_ms() > deadline)
                return SDIO_ERR_TIMEOUT;
        } while(!(resp & R1_READY_FOR_DATA));
    }

    return SDIO_OK;
}

uint32_t sdio_get_sector_count (void)
{
    return card_sectors;
}

bool sdio_card_ready (void)
{
    return card_ready;
}

/*******************************************************
 * FatFs block device ops (diskio registration)
 *******************************************************/

static volatile DSTATUS sdio_stat = STA_NOINIT;

static DSTATUS sdio_ops_initialize (void)
{
    if(sdio_init() == SDIO_OK)
        sdio_stat &= ~STA_NOINIT;
    else
        sdio_stat = STA_NOINIT;

    return sdio_stat;
}

static DSTATUS sdio_ops_status (void)
{
    return sdio_stat;
}

static DRESULT sdio_ops_read (BYTE *buff, DWORD sector, BYTE count)
{
    if(sdio_stat & STA_NOINIT)
        return RES_NOTRDY;

    return sdio_read_sectors((uint8_t *)buff, (uint32_t)sector, (uint32_t)count) == SDIO_OK
            ? RES_OK : RES_ERROR;
}

#if FF_FS_READONLY == 0
static DRESULT sdio_ops_write (const BYTE *buff, DWORD sector, BYTE count)
{
    if(sdio_stat & STA_NOINIT)
        return RES_NOTRDY;

    return sdio_write_sectors((const uint8_t *)buff, (uint32_t)sector, (uint32_t)count) == SDIO_OK
            ? RES_OK : RES_ERROR;
}
#endif

static DRESULT sdio_ops_ioctl (BYTE cmd, void *buff)
{
    switch(cmd) {

        case CTRL_SYNC:
            return RES_OK;

        case GET_SECTOR_COUNT:
            *(DWORD *)buff = (DWORD)sdio_get_sector_count();
            return *(DWORD *)buff ? RES_OK : RES_ERROR;

        case GET_SECTOR_SIZE:
            *(WORD *)buff = SDIO_BLOCK_SIZE;
            return RES_OK;

        case GET_BLOCK_SIZE:
            *(DWORD *)buff = 1;
            return RES_OK;

        default:
            break;
    }

    return RES_PARERR;
}

static const diskio_ops_t sdio_ops = {
    .initialize = sdio_ops_initialize,
    .status     = sdio_ops_status,
    .read       = sdio_ops_read,
#if FF_FS_READONLY == 0
    .write      = sdio_ops_write,
#else
    .write      = NULL,
#endif
    .ioctl      = sdio_ops_ioctl,
    .timerproc  = sdio_timerproc
};

void sdio_register (uint8_t pdrv)
{
    diskio_register((BYTE)pdrv, &sdio_ops);
}

#endif // SDCARD_ENABLE && SDCARD_SDIO
