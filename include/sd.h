/********************************************************************
 * sd.h
 *
 *  SD card driver header for Raspberry Pi.
 *
 *  April 2024
 *
 *******************************************************************/

#ifndef __SD_H__
#define __SD_H__

#include    <stdint.h>

#include    "errors.h"

/********************************************************************
 *  SD card module interface
 */

error_t sd_init(void);
void    sd_close(void);

error_t sd_read_block(uint32_t lba, uint8_t *buffer, uint16_t length);
error_t sd_write_block(uint32_t lba, uint8_t *buffer, uint16_t length);

#endif  /* __SD_H__ */
