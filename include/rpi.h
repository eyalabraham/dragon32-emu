/********************************************************************
 * rpi.h
 *
 *  Functions and definitions for RPi machine-dependent functionality.
 *  This header is common to Bare-metal and Linux OS
 *
 *  July 2024
 *
 *******************************************************************/

#ifndef __RPI_H__
#define __RPI_H__

#include    <stdint.h>

#define     DEFAULT_UART_RATE   BAUD_115200
#define     DEFAULT_SPI0_RATE   2000000     // Keyboard interface Hz bit rate
#define     MOTOR_LED_DISK      0b00000001
#define     MOTOR_LED_TAPE      0b00000010

/********************************************************************
 *  RPi bare meta module API
 */
int      rpi_gpio_init(void);

uint8_t *rpi_fb_init(int h, int v);

uint32_t rpi_system_timer(void);

int      rpi_keyboard_read(void);
void     rpi_keyboard_reset(void);

int      rpi_joystk_comp(void);
int      rpi_rjoystk_button(void);

int      rpi_reset_button(void);

void     rpi_audio_mux_set(int);

void     rpi_write_dac(int);

void     rpi_disable(void);
void     rpi_enable(void);

void     rpi_motor_led_on(uint8_t);
void     rpi_motor_led_off(uint8_t);

void     rpi_testpoint_on(void);
void     rpi_testpoint_off(void);

void     rpi_halt(void);

//void     _putchar(char character); listed for clarity, does not need to be defined 

#endif  /* __RPI_H__ */
