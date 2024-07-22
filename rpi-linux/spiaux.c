/********************************************************************
 * spiaux.c
 *
 *  SPI1 platform dependent driver for Raspberry Pi.
 *  Uses bcm2835 library by Mike McCauley @ https://www.airspayce.com/mikem/bcm2835/index.html
 *
 *  April 2024
 *
 *******************************************************************/

#include    <time.h>

#include    "rpi-linux/spiaux.h"

/* -----------------------------------------
   Local definitions
----------------------------------------- */

/* -----------------------------------------
   Module static functions
----------------------------------------- */

/* -----------------------------------------
   Module globals
----------------------------------------- */
static  int     spi_aux_initialized = 0;

/*------------------------------------------------
 * spi_aux_init()
 *
 *  Initialize RPi GPIO functions:
 *  - Serial interface to AVR (PS2 keyboard controller)
 *  - Reset output GPIO to AVR
 *  - Output timing test point
 *  - Output GPIO bits to 6-bit DAC
 *
 *  param:  None
 *  return: -1 fail, 0 ok
 */
error_t spi_aux_init(void)
{
    /* This seems to be safe to do.
     * Keep this here just in case fat32_init() is called before rpi_gpio_init()
     */
    if (!bcm2835_init())
    {
      return SPI_GPIO_SYS_INIT;
    }

    if (!bcm2835_aux_spi_begin())
    {
      return SPI_INIT;
    }

    /* Setup SPI with defaults for an SD card
     */
    bcm2835_spi_setBitOrder(BCM2835_SPI_BIT_ORDER_MSBFIRST);
    bcm2835_spi_setDataMode(BCM2835_SPI_MODE0);
    bcm2835_spi_chipSelect(BCM2835_SPI_CS0);
    bcm2835_spi_setChipSelectPolarity(BCM2835_SPI_CS0, LOW);

    spi_aux_initialized = 1;

    return NO_ERROR;
}

/*------------------------------------------------
 * spi_aux_close()
 *
 *  Close RPi GPIO subsystem and return to defaults.
 *
 *  param:  None
 *  return: None
 */
void spi_aux_close(void)
{
    if ( !spi_aux_initialized )
        return;

    bcm2835_spi_end();
    bcm2835_close();
    spi_aux_initialized = 0;
}

/*------------------------------------------------
 * spi_aux_set_rate()
 *
 *  Set AUX API data bit rate.
 *
 *  param:  data rate in Hz
 *  return: None
 */
void spi_aux_set_rate(uint32_t data_rate)
{
    bcm2835_aux_spi_setClockDivider(bcm2835_aux_spi_CalcClockDivider(data_rate));
}

/*------------------------------------------------
 * spi_aux_transfer_byte()
 *
 *  Transfer one bytes to/from SPI0.
 *
 *  param:  Byte to send
 *  return: Byte received
 */
uint8_t spi_aux_transfer_byte(uint8_t value)
{
    if ( !spi_aux_initialized )
        return 0;

    return bcm2835_aux_spi_transfer(value);
}

/*------------------------------------------------
 * spi_aux_transfer_buffer()
 *
 *  Transfer any number of bytes (up to 65536) to/from
 *  a buffer from/to SPI0.
 *
 *  param:  Buffer pointer, length (1 to 65536)
 *          Received bytes will replace the contents.
 *  return: None
 */
void spi_aux_transfer_buffer(uint8_t* buffer, uint16_t len)
{
    if ( !spi_aux_initialized )
        return;

    if ( len > 0 )
    {
        bcm2835_aux_spi_transfern((char*) buffer, (uint32_t) len);
    }
}

/*------------------------------------------------
 * spi_aux_set_cs_high()
 *
 *  Force SPI CS line to high level.
 *
 *  param:  None
 *  return: None
 */
void spi_aux_set_cs_high(void)
{
    if ( !spi_aux_initialized )
        return;

    bcm2835_gpio_fsel(SPI_AUX_SD_CS, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_set(SPI_AUX_SD_CS);
}

/*------------------------------------------------
 * spi_aux_set_cs_spi_func()
 *
 *  Force SPI CS line to function as SPI CS line.
 *
 *  param:  None
 *  return: None
 */
void spi_aux_set_cs_spi_func(void)
{
    if ( !spi_aux_initialized )
        return;

    bcm2835_gpio_fsel(SPI_AUX_SD_CS, BCM2835_GPIO_FSEL_ALT4);
}

/*------------------------------------------------
 * spi_aux_delay()
 *
 *  Milisecond delay
 *
 *  param:  None
 *  return: None
 */
void spi_aux_delay(uint32_t delay)
{
    bcm2835_delay(delay);
}