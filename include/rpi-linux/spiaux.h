/********************************************************************
 * spiaux.h
 *
 *  SPI1 platform dependent driver header for Raspberry Pi.
 *  Uses bcm2835 library by Mike McCauley @ https://www.airspayce.com/mikem/bcm2835/index.html
 *
 *  April 2024
 *
 *******************************************************************/

#ifndef __SPIAUX_H__
#define __SPIAUX_H__

#include    <stdint.h>

#include    "rpi-linux/bcm2835.h"
#include    "errors.h"

/* -----------------------------------------
   Module definitions
----------------------------------------- */

#define     SPI_AUX_SD_CS               RPI_V2_GPIO_P1_36

/********************************************************************
 *  SPI module interface
 */

error_t     spi_aux_init(void);
void        spi_aux_close(void);
void        spi_aux_set_rate(uint32_t);

uint8_t     spi_aux_transfer_byte(uint8_t);
void        spi_aux_transfer_buffer(uint8_t*, uint16_t);

void        spi_aux_set_cs_high(void);
void        spi_aux_set_cs_spi_func(void);

void        spi_aux_delay(uint32_t);

#endif  /* __SPIAUX_H__ */
