/********************************************************************
 * uart.h
 *
 *  UART interface module.
 *
 *  January 29 1, 2021
 *
 *******************************************************************/

#ifndef __uart_h__
#define __uart_h__

#include    <stdint.h>
#include    <termios.h>

#define     UART_UART0          "/dev/serial0"  // default serial link on RPi Zero
#define     UART_BAUD           B57600
#define     UART_BITS           8

/********************************************************************
 * Function prototypes
 *
 */
int      uart_init(void);
void     uart_close(void);
int      uart_recv(void);
void     uart_send(uint8_t);
int      uart_flush(void);

#endif      /* __uart_h__ */
