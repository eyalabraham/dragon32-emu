/********************************************************************
 * uart.c
 *
 *  UART interface module.
 *
 *  January 29, 2021
 *
 *******************************************************************/

#include    <stdio.h>
#include    <string.h>
#include    <unistd.h>
#include    <fcntl.h>
#include    <errno.h>

#include    "rpi-linux/uart.h"

/********************************************************************
 * Module static functions
 *
 */
static int uart_set_interface_attr(int, int);
static int uart_set_blocking(int, int);

/********************************************************************
 * Module globals
 *
 */
static int   uart_fd;

/********************************************************************
 * uart_init()
 *
 * Initialize the UART and GPIO subsystems of BCM2835.
 * Failure to initialize any of the above three IO subsystems
 * will result in closing all open IO devices and exiting with an error.
 *
 *  param:  none
 *  return: 0 if no error,
 *         -1 if error initializing any subcomponent or library
 *
 */
int uart_init(void)
{
    // open UART0 port
    uart_fd = open(UART_UART0, O_RDWR | O_NOCTTY | O_NDELAY);
    if ( uart_fd == -1 )
    {
        printf("%s: Error %d opening %s\n", __FUNCTION__, errno, UART_UART0);
        return -1;
    }
    else
    {
        // Setup UART options
        uart_set_interface_attr(uart_fd, UART_BAUD);
        uart_set_blocking(uart_fd, 0);
        fcntl(uart_fd, F_SETFL, FNDELAY);

        printf("Initialized UART0 %s\n", UART_UART0);
    }

    return 0;
}

/********************************************************************
 * uart_close()
 *
 *  Close the UART and RPi GPIO interfaces with the PCXT.
 *
 *  param:  none
 *  return: none
 */
void uart_close(void)
{
    close(uart_fd);
}

/********************************************************************
 * uart_recv()
 *
 *  Check UART and return received byte.
 *
 *  param:  none
 *  return: Bytes received
 *         -1 if error
 */
int uart_recv(void)
{
    int     read_result;
    uint8_t c;

    read_result = read(uart_fd, &c, 1);

    // exit if nothing read or time out
    if ( read_result == -1 && errno == EAGAIN )
    {
        return -1;
    }
    // exit on IO error
    else if ( read_result == -1 && errno != EAGAIN )
    {
        printf("%s: Error %d reading UART\n", __FUNCTION__, errno);
        return -1;
    }

    return c;
}


/********************************************************************
 * uart_send()
 *
 *  Send a data byte to host PC/XT.
 *
 *  param:  data byte to send
 *  return: none
 */
void uart_send(uint8_t byte)
{
    write(uart_fd, (void*) &byte, 1);
}

/********************************************************************
 * uart_flush()
 *
 *  Flush UART buffer.
 *
 *  param:  none
 *  return: '0' on success. '-1' on failure and set errno to indicate the error
 *
 */
int uart_flush(void)
{
    return tcflush(uart_fd, TCIOFLUSH);
}

/********************************************************************
 * uart_set_interface_attr()
 *
 *  Initialize UART attributes.
 *
 *  param:  file descriptor, baud rate, and parity type (to enable: PARENB + for odd: PARODD)
 *  return: 0 if no error,
 *         -1 if error initializing
 *
 */
int uart_set_interface_attr(int fd, int speed)
{
    struct termios tty;

    memset (&tty, 0, sizeof tty);
    if (tcgetattr(fd, &tty) != 0)
    {
        printf("%s: Error %d from tcgetattr", __FUNCTION__, errno);
        return -1;
    }

    cfsetospeed (&tty, speed);
    cfsetispeed (&tty, speed);

    tty.c_iflag = IGNBRK;                           // ignore break, no xon/xoff

    tty.c_oflag = 0;

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;     // 8-bit chars
    tty.c_cflag |= CLOCAL;                          // ignore modem controls
    tty.c_cflag |= CREAD;                           // enable reading
    tty.c_cflag &= ~(PARENB | PARODD);              // no parity
    tty.c_cflag &= ~CSTOPB;                         // one stop bit
    tty.c_cflag &= ~CRTSCTS;                        // no flow control

    tty.c_lflag = 0;                                // no signaling chars, no echo, no canonical processing

    if (tcsetattr (fd, TCSANOW, &tty) != 0)
    {
        printf("%s: Error %d from tcsetattr()", __FUNCTION__, errno);
        return -1;
    }

    printf("UART settings:\n\tc_iflag=0x%x\n\tc_oflag=0x%x\n\tc_cflag=0x%x\n\tc_lflag=0x%x\n",
            tty.c_iflag, tty.c_oflag, tty.c_cflag, tty.c_lflag);

    return 0;
}

/********************************************************************
 * uart_set_blocking()
 *
 *  Initialize UART attributes.
 *
 *  param:  file descriptor, '0' non-blocking read or '1' blocking read
 *  return: 0 if no error,
 *         -1 if error initializing
 *
 */
int uart_set_blocking(int fd, int should_block)
{
    struct termios tty;

    memset (&tty, 0, sizeof tty);
    if (tcgetattr (fd, &tty) != 0)
    {
        printf("%s: Error %d from tggetattr()", __FUNCTION__, errno);
        return -1;
    }

    tty.c_cc[VMIN]  = should_block ? 1 : 0;
    tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

    if (tcsetattr (fd, TCSANOW, &tty) != 0)
    {
        printf("%s: Error %d setting term attributes", __FUNCTION__, errno);
        return -1;
    }

    return 0;
}
