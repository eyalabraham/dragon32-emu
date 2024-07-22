/********************************************************************
 * errors.h
 *
 *  System-wide errors.
 *
 *  April 2024
 *
 *******************************************************************/

#ifndef __ERRORS_H__
#define __ERRORS_H__

typedef enum
    {
        NO_ERROR = 0,

        SPI_GPIO_SYS_INIT = -1,         // bcm2835_init() failed. Are you running as root?
        SPI_INIT = -2,                  // bcm2835_spi_begin() failed. Are you running as root?

        SD_SPI_FAIL = -3,
        SD_FAIL_READY = -4,             // sd_init(): Time out waiting for SD ready state.
        SD_FAIL_IDLE = -5,              // sd_init(): SD card failed SD_GO_IDLE_STATE.
        SD_FAIL_APP_CMD = -6,           // sd_init(): SD card failed SD_APP_CMD.
        SD_FAIL_OP_COND = -7,           // sd_init(): SD card failed SD_APP_SEND_OP_COND.
        SD_NOT_R1_READY = -8,           // sd_init(): SD card failed, not in R1 ready state.
        SD_FAIL_SET_BLOCKLEN = -9,      // sd_init(): SD card failed setting block size.
        SD_TIMEOUT = -10,
        SD_BAD_CRC = -11,               // CRC error
        SD_READ_FAIL = -12,             // Read data error.
        SD_WRITE_FAIL = -13,            // Write data error.

        FAT_READ_FAIL = -14,
        FAT_WRITE_FAIL = -15,
        FAT_BAD_SECTOR_SIG = -16,       // Bad sector signature (expecting 0x55 or 0xaa).
        FAT_BAD_PARTITION_TYPE = -17,   // Bad partition type (expecting 0x0c FAT32).
        FAT_BAD_SECTOR_PER_CLUS = -18,
        FAT_FILE_NOT_OPEN = -19,
        FAT_FILE_SEEK_RANGE = -20,      // Seek out of range
        FAT_FILE_SEEK_ERR = -21,        // Unexpected FAT32 entry during seek
        FAT_FILE_OPEN_ERR = -22,        // File could not be opened
        FAT_EOF = -23,
        FAT_FILE_NOT_FOUND = -24,
        FAT_OUT_OF_SPACE = -25,         // No more room on media, FAT full
        FAT_CRITICAL_ERR = -26,         // FAT or data corruption possible
    } error_t;

#endif  /* __ERRORS_H__ */
