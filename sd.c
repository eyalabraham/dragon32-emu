/********************************************************************
 * sd.c
 *
 *  SD card driver for Raspberry Pi.
 *  Resource: http://elm-chan.org/docs/mmc/mmc_e.html
 *
 *  April 2024
 *
 *******************************************************************/

#include    <string.h>

#if (RPI_BARE_METAL==0)
    #include    <time.h>
    #include    "rpi-linux/spiaux.h"
#else
    #include    "rpi-bm/gpio.h"
    #include    "rpi-bm/timer.h"
    #include    "rpi-bm/spi1.h"
#endif
#include    "sd.h"
#include    "config.h"

/* -----------------------------------------
   Local definitions
----------------------------------------- */

// SD card
#define     SPI_FILL_BYTE           0xff

#define     SD_CMD0                 0
#define     SD_CMD1                 1
#define     SD_CMD8                 8
#define     SD_CMD9                 9
#define     SD_CMD10                10
#define     SD_CMD12                12
#define     SD_CMD16                16
#define     SD_CMD17                17
#define     SD_CMD18                18
#define     SD_CMD23                23
#define     SD_CMD24                24
#define     SD_CMD25                25
#define     SD_CMD55                55
#define     SD_CMD58                58
#define     SD_CMD59                59
#define     SD_ACMD41               41

#define     SD_GO_IDLE_STATE        SD_CMD0
#define     SD_SEND_OP_COND         SD_CMD1
#define     SD_SEND_IF_COND         SD_CMD8
#define     SD_SEND_CSD             SD_CMD9
#define     SD_SEND_CID             SD_CMD10
#define     SD_STOP_TRANSMISSION    SD_CMD12
#define     SD_SET_BLOCKLEN         SD_CMD16
#define     SD_READ_SINGLE_BLOCK    SD_CMD17
#define     SD_READ_MULTIPLE_BLOCK  SD_CMD18
#define     SD_SET_BLOCK_COUNT      SD_CMD23
#define     SD_WRITE_BLOCK          SD_CMD24
#define     SD_WRITE_MULTIPLE_BLOCK SD_CMD25
#define     SD_APP_CMD              SD_CMD55
#define     SD_READ_OCR             SD_CMD58
#define     SD_NO_CRC               SD_CMD59
#define     SD_APP_SEND_OP_COND     SD_ACMD41

#define     SD_TOKEN_START_BLOCK    0xfe        // For CMD17/18/24
#define     SD_TOKEN_CMD25          0xfc        // For CMD25, writ emultiple blocks
#define     SD_TOKEN_STOP_TX        0xfd        // Stop transmitting data blocks after CMD25

#define     SD_DATA_RESP_ACK        0x05        // Write data Accpted
#define     SD_DATA_RESP_CRC_ERR    0x0b        // Write data CRC error
#define     SD_DATA_RESP_REJECT     0x0d        // Write data rejected, write error

#define     SD_R1_READY             0b00000000
#define     SD_R1_IDLE              0b00000001
#define     SD_R1_ERASE_RESET       0b00000010
#define     SD_R1_ILLIGAL_CMD       0b00000100
#define     SD_R1_CRC_ERROR         0b00001000
#define     SD_R1_ERASE_ERROR       0b00010000
#define     SD_R1_ADDRESS_ERROR     0b00100000
#define     SD_R1_PARAM_ERROR       0b01000000
#define     SD_FAILURE              0xff

#define     SD_BLOCK_SIZE           512         // Bytes
#define     SD_TIME_OUT             500000      // 500mSec
#define     SD_NCR                  10          // Command response time: 0 to 8 bytes for SDC, 1 to 8 bytes for MMC

/* Aliases for compatibility between
 * bare-metal and OS libraries.
 */
#if (RPI_BARE_METAL==1)
    #define     clock()                         bcm2835_st_read()
    #define     spi_aux_close()                 bcm2835_spi1_close()
    #define     spi_aux_transfer_byte(a)        bcm2835_spi1_transfer_byte(a)
    #define     spi_aux_transfer_buffer(a,b)    bcm2835_spi1_transfer_Ex(a,a,b)
#endif

/* -----------------------------------------
   Module static functions
----------------------------------------- */
static uint8_t   sd_send_cmd(uint8_t cmd, uint32_t arg);
static int       sd_wait_token(uint8_t token);
static int       sd_wait_ready(void);
static uint8_t   sd_get_crc7(uint8_t *message, uint16_t length);
static uint16_t  sd_get_crc16(const uint8_t *buf, uint16_t length );

/* -----------------------------------------
   Module globals
----------------------------------------- */
static int      sd_initialized = 0;

/* -------------------------------------------------------------
 * sd_init()
 *
 *  Initialize SD card and reader
 *
 *  Param:  None
 *  Return: Driver error
 */
error_t sd_init(void)
{
    uint8_t     mosi_buffer[10] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    uint8_t     result;
    uint32_t    start_time;

    /* SD card initialization sequence
     * Resource: http://elm-chan.org
     */
#if (RPI_BARE_METAL==0)
    if ((result = spi_aux_init()) != NO_ERROR)
    {
        return result;
    }

    spi_aux_set_rate(SD_CARD_BIT_RATE);

    spi_aux_set_cs_high();                          // CS to 'High'

    spi_aux_delay(2);                               // Power-on delay
    spi_aux_transfer_buffer(mosi_buffer, 10);       // Dummy clocks, CS=DI=High
    spi_aux_set_cs_spi_func();                      // Back to normal CS
#else
    if ( !bcm2835_spi1_init(SPI1_DEFAULT) )
    {
      return SPI_INIT;
    }

    bcm2835_spi1_set_rate(SD_CARD_BIT_RATE);

    bcm2835_spi1_set_cs_high();

    bcm2835_st_delay(2000);                         // Power on delay
    bcm2835_spi1_transfer_Ex(mosi_buffer, 0, 10);   // Dummy clocks, CS=DI=High
    bcm2835_spi1_set_cs_spi_func();             // Back to normal CS
#endif

    if ( sd_wait_ready() == 0 )                     // Check MISO is high (card DO=1)
    {
        return SD_FAIL_READY;
    }

    result = sd_send_cmd(SD_GO_IDLE_STATE, 0);
    if ( result != SD_R1_IDLE )
    {
        return SD_FAIL_IDLE;
    }

    start_time = (uint32_t) clock();

    do
    {
        result = sd_send_cmd(SD_APP_CMD, 0);
        if ( result == SD_FAILURE )
        {
            return SD_FAIL_APP_CMD;
        }

        result = sd_send_cmd(SD_APP_SEND_OP_COND, 0);
        if ( result == SD_FAILURE )
        {
            return SD_FAIL_OP_COND;
        }
    }
    while ( result != SD_R1_READY && ((uint32_t) clock() - start_time) < SD_TIME_OUT );

    if ( result != SD_R1_READY )
    {
        return SD_NOT_R1_READY;
    }

    result = sd_send_cmd(SD_SET_BLOCKLEN, SD_BLOCK_SIZE);
    if ( result != SD_R1_READY )
    {
        return SD_FAIL_SET_BLOCKLEN;
    }

    sd_initialized = 1;

    return NO_ERROR;
}

/* -------------------------------------------------------------
 * sd_close()
 *
 *  Close SD reader
 *
 *  Param:  None
 *  Return: None
 */
void sd_close(void)
{
    if ( !sd_initialized )
        return;

    spi_aux_close();
    sd_initialized = 0;
}

/* -------------------------------------------------------------
 * sd_read_block()
 *
 *  Read a block (sector) from the SD card
 *
 *  Param:  LBA number, buffer address, and its length
 *  Return: Driver error
 */
error_t sd_read_block(uint32_t lba, uint8_t *buffer, uint16_t length)
{
    uint8_t sd_response;
    uint8_t crc_high, crc_low;

    if ( length < SD_BLOCK_SIZE ||  !sd_initialized )
    {
        return SD_READ_FAIL;
    }

    /* check if DO is high
     */
    if ( sd_wait_ready() == 0 )
    {
        return SD_FAIL_READY;
    }

    /* Send read command to SD card
     */
    memset(buffer, SPI_FILL_BYTE, SD_BLOCK_SIZE);

    sd_response = sd_send_cmd(SD_READ_SINGLE_BLOCK, lba * SD_BLOCK_SIZE);   // *** SDC uses BYTE addressing ***
    if ( sd_response != SD_R1_READY )
    {
        return SD_READ_FAIL;
    }

    /* Wait for start of data token (0xFE)
     */
    if ( sd_wait_token(SD_TOKEN_START_BLOCK) == 0 )
    {
        return SD_TIMEOUT;
    }

    /* Read a data block (one SD sector)
     */
    spi_aux_transfer_buffer(buffer, SD_BLOCK_SIZE);
    crc_high = spi_aux_transfer_byte(SPI_FILL_BYTE);
    crc_low = spi_aux_transfer_byte(SPI_FILL_BYTE);

    /* Check CRC
     */
    if ( sd_get_crc16(buffer, SD_BLOCK_SIZE) != ((crc_high << 8) + crc_low) )
    {
        return SD_BAD_CRC;
    }

    return NO_ERROR;
}

/* -------------------------------------------------------------
 * sd_write_block()
 *
 *  Write a block (sector) to SD card.
 *  Data buffer must be at least SD_BLOCK_SIZE in size,
 *  if larger, only the first SD_BLOCK_SIZE bytes will be written.
 *
 *  Param:  LBA number, buffer address, and its length
 *  Return: Driver error
 */
error_t sd_write_block(uint32_t lba, uint8_t *buffer, uint16_t length)
{
    uint8_t sd_response;
    uint16_t crc;

    static uint8_t temp_buffer[SD_BLOCK_SIZE];

    if ( length < SD_BLOCK_SIZE ||  !sd_initialized )
    {
        return SD_WRITE_FAIL;
    }

    /* check if DO is high
     */
    if ( sd_wait_ready() == 0 )
    {
        return SD_FAIL_READY;
    }

    /* Send write command to SD card
     */
    sd_response = sd_send_cmd(SD_WRITE_BLOCK, lba * SD_BLOCK_SIZE);   // *** SDC uses BYTE addressing ***
    if ( sd_response != SD_R1_READY )
    {
        return SD_WRITE_FAIL;
    }

    /* Copy data to a temporary buffer becasue SPI writes clobber
     * existing data in the buffer.
     */
    memcpy(temp_buffer, buffer, SD_BLOCK_SIZE);

    spi_aux_transfer_byte(SPI_FILL_BYTE);
    spi_aux_transfer_byte(SPI_FILL_BYTE);

    /* Write a data block (one SD sector)
     */
    crc = sd_get_crc16(buffer, SD_BLOCK_SIZE);

    spi_aux_transfer_byte(SD_TOKEN_START_BLOCK);
    spi_aux_transfer_buffer(temp_buffer, SD_BLOCK_SIZE);
    spi_aux_transfer_byte((crc >> 8) & 0xff);
    spi_aux_transfer_byte(crc & 0xff);

    /* Wait for write status
     */
    sd_response = spi_aux_transfer_byte(SPI_FILL_BYTE) & 0x0f;
    if ( sd_response == SD_DATA_RESP_CRC_ERR ||
         sd_response == SD_DATA_RESP_REJECT )
    {
        return SD_WRITE_FAIL;
    }

    return NO_ERROR;
}

/* -------------------------------------------------------------
 * sd_send_cmd()
 *
 *  Send a command to the SD card
 *
 *  Param:  Command code and argument
 *  Return: Failure=SD_FAILURE, or R1 SD status
 */
static uint8_t sd_send_cmd(uint8_t cmd, uint32_t arg)
{
    int     i;
    uint8_t response;
    uint8_t mosi_buffer[6];
    uint8_t crc;

    /* check if DO is high
     */
    if ( sd_wait_ready() == 0 )
    {
        return SD_FAIL_READY;
    }

    /* Prepare the command packet and
     * always provide a correct CRC
     */
    mosi_buffer[0] = 0x40 | cmd;
    mosi_buffer[1] = (uint8_t)(arg >> 24);
    mosi_buffer[2] = (uint8_t)(arg >> 16);
    mosi_buffer[3] = (uint8_t)(arg >> 8);
    mosi_buffer[4] = (uint8_t)(arg >> 0);

    crc = sd_get_crc7(mosi_buffer, 5);
    mosi_buffer[5] = (crc << 1) | 0b00000001;

    /* Send the command command packet via SPI.
     */
    spi_aux_transfer_buffer(mosi_buffer, sizeof(mosi_buffer));

    /* Send out '1's on MOSI until a response is received from the SD
     */
    i = 0;
    do
    {
        response = spi_aux_transfer_byte(SPI_FILL_BYTE);
        i++;
    }
    while ( response == SPI_FILL_BYTE && i < SD_NCR );

    return response;
}

/* -------------------------------------------------------------
 * sd_wait_token()
 *
 *  Wait for a data token from the SD card
 *
 *  Param:  Token to wait for
 *  Return: ok=1, time-out=0
 */
static int sd_wait_token(uint8_t token)
{
    uint32_t    start_time;
    uint8_t     have_token;

    start_time = (uint32_t) clock();
    have_token = 0;

    do
    {
        if ( spi_aux_transfer_byte(SPI_FILL_BYTE) == token )
        {
            have_token = 1;
            break;
        }
    }
    while ( ((uint32_t)clock() - start_time) < SD_TIME_OUT );

    return have_token;
}

/* -------------------------------------------------------------
 * sd_wait_ready()
 *
 *  Wait for a ready state (DO=1) from the SD card
 *
 *  Param:  None
 *  Return: ready=1, time-out=0
 */
static int sd_wait_ready(void)
{
    uint32_t    start_time;

    start_time = (uint32_t) clock();

    do
    {
        if ( SPI_FILL_BYTE == spi_aux_transfer_byte(SPI_FILL_BYTE) )
        {
            return 1;
        }
    }
    while ( ((uint32_t)clock() - start_time) < SD_TIME_OUT );

    return 0;
}

/* -------------------------------------------------------------
 * sd_get_crc7()
 *
 *  Calculate CRC7 on a message.
 *
 *  Param:  Pointer to message buffer, length of message
 *  Return: CRC7 byte
 */
static uint8_t sd_get_crc7(uint8_t *message, uint16_t length)
{
    static uint8_t crc_lookup_table[256] =
        {
            0x00, 0x09, 0x12, 0x1b, 0x24, 0x2d, 0x36, 0x3f, 0x48, 0x41, 0x5a, 0x53, 0x6c, 0x65, 0x7e, 0x77,
            0x19, 0x10, 0x0b, 0x02, 0x3d, 0x34, 0x2f, 0x26, 0x51, 0x58, 0x43, 0x4a, 0x75, 0x7c, 0x67, 0x6e,
            0x32, 0x3b, 0x20, 0x29, 0x16, 0x1f, 0x04, 0x0d, 0x7a, 0x73, 0x68, 0x61, 0x5e, 0x57, 0x4c, 0x45,
            0x2b, 0x22, 0x39, 0x30, 0x0f, 0x06, 0x1d, 0x14, 0x63, 0x6a, 0x71, 0x78, 0x47, 0x4e, 0x55, 0x5c,
            0x64, 0x6d, 0x76, 0x7f, 0x40, 0x49, 0x52, 0x5b, 0x2c, 0x25, 0x3e, 0x37, 0x08, 0x01, 0x1a, 0x13,
            0x7d, 0x74, 0x6f, 0x66, 0x59, 0x50, 0x4b, 0x42, 0x35, 0x3c, 0x27, 0x2e, 0x11, 0x18, 0x03, 0x0a,
            0x56, 0x5f, 0x44, 0x4d, 0x72, 0x7b, 0x60, 0x69, 0x1e, 0x17, 0x0c, 0x05, 0x3a, 0x33, 0x28, 0x21,
            0x4f, 0x46, 0x5d, 0x54, 0x6b, 0x62, 0x79, 0x70, 0x07, 0x0e, 0x15, 0x1c, 0x23, 0x2a, 0x31, 0x38,
            0x41, 0x48, 0x53, 0x5a, 0x65, 0x6c, 0x77, 0x7e, 0x09, 0x00, 0x1b, 0x12, 0x2d, 0x24, 0x3f, 0x36,
            0x58, 0x51, 0x4a, 0x43, 0x7c, 0x75, 0x6e, 0x67, 0x10, 0x19, 0x02, 0x0b, 0x34, 0x3d, 0x26, 0x2f,
            0x73, 0x7a, 0x61, 0x68, 0x57, 0x5e, 0x45, 0x4c, 0x3b, 0x32, 0x29, 0x20, 0x1f, 0x16, 0x0d, 0x04,
            0x6a, 0x63, 0x78, 0x71, 0x4e, 0x47, 0x5c, 0x55, 0x22, 0x2b, 0x30, 0x39, 0x06, 0x0f, 0x14, 0x1d,
            0x25, 0x2c, 0x37, 0x3e, 0x01, 0x08, 0x13, 0x1a, 0x6d, 0x64, 0x7f, 0x76, 0x49, 0x40, 0x5b, 0x52,
            0x3c, 0x35, 0x2e, 0x27, 0x18, 0x11, 0x0a, 0x03, 0x74, 0x7d, 0x66, 0x6f, 0x50, 0x59, 0x42, 0x4b,
            0x17, 0x1e, 0x05, 0x0c, 0x33, 0x3a, 0x21, 0x28, 0x5f, 0x56, 0x4d, 0x44, 0x7b, 0x72, 0x69, 0x60,
            0x0e, 0x07, 0x1c, 0x15, 0x2a, 0x23, 0x38, 0x31, 0x46, 0x4f, 0x54, 0x5d, 0x62, 0x6b, 0x70, 0x79
        };

    uint16_t    i;
    uint8_t     crc = 0;

    for ( i = 0; i < length; i++ )
        crc = crc_lookup_table[(crc << 1) ^ message[i]];

    return crc;
}

/* -------------------------------------------------------------
 * sd_get_crc16()
 *
 *  Calculate CRC16 on a message.
 *
 *  Param:  Pointer to message buffer, length of message
 *  Return: CRC16 word
 */
static uint16_t sd_get_crc16(const uint8_t *buf, uint16_t length )
{
    uint16_t crc = 0;
    int i;

    while( length-- )
    {
        crc ^= *(uint8_t *)buf++ << 8;
        for( i = 0; i < 8; i++ )
        {
            if( crc & 0x8000 )
                crc = (crc << 1) ^ 0x1021;
            else
                crc = crc << 1;
        }
    }
    return crc;
}
