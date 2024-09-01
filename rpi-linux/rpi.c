/********************************************************************
 * rpi.c
 *
 *  Functions and definitions for RPi machine-dependent functionality.
 *  This is the Linux implementation (*not* bare-metal option.)
 *
 *  July 2024
 *
 *******************************************************************/

#include    <stdio.h>
#include    <time.h>
#include    <assert.h>
#include    <fcntl.h>
#include    <linux/fb.h>
#include    <linux/kd.h>
#include    <sys/mman.h>
#include    <sys/ioctl.h>

#include    <unistd.h>
#include    <string.h>

#include    "rpi-linux/bcm2835.h"
#include    "dbgmsg.h"
#include    "rpi.h"

/* -----------------------------------------
   Local definitions
----------------------------------------- */
// AVR and keyboard
#define     AVR_RESET           RPI_V2_GPIO_P1_11
#define     TEST_POINT          RPI_V2_GPIO_P1_07
#define     MOTOR_LED           RPI_V2_GPIO_P1_12

// Miscellaneous IO
#define     EMULATOR_RESET      RPI_V2_GPIO_P1_29

// Audio multiplexer and DAC/ADC
#define     AUDIO_MUX0          RPI_V2_GPIO_P1_03
#define     AUDIO_MUX1          RPI_V2_GPIO_P1_05
#define     AUDIO_MUX_MASK      ((1 << AUDIO_MUX0) | (1 << AUDIO_MUX1))

#define     DAC_BIT0            RPI_V2_GPIO_P1_15
#define     DAC_BIT1            RPI_V2_GPIO_P1_16
#define     DAC_BIT2            RPI_V2_GPIO_P1_18
#define     DAC_BIT3            RPI_V2_GPIO_P1_22
#define     DAC_BIT4            RPI_V2_GPIO_P1_37
#define     DAC_BIT5            RPI_V2_GPIO_P1_13

#define     JOYSTK_COMP         RPI_V2_GPIO_P1_26   // Joystick
#define     JOYSTK_BUTTON       RPI_V2_GPIO_P1_24   // Joystick button

#define     DAC_BIT_MASK        ((1 << DAC_BIT0) | (1 << DAC_BIT1) | (1 << DAC_BIT2) | \
                                 (1 << DAC_BIT3) | (1 << DAC_BIT4) | (1 << DAC_BIT5))

/* -----------------------------------------
   Module static functions
----------------------------------------- */
static uint8_t  *fb_set_resolution(int fbh, int x_pix, int y_pix);
static int       fb_set_tty(const int mode);

/* -----------------------------------------
   Module globals
----------------------------------------- */
static int      fbfd = 0;                       // frame buffer file descriptor
static uint8_t  motor_led_ctrl = 0;             // Holds the source and state of LED

/*------------------------------------------------
 * rpi_gpio_init()
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
int rpi_gpio_init(void)
{
    if (!bcm2835_init())
    {
      dbg_printf(0, "rpi_gpio_init()[%d]: bcm2835_init failed. Are you running as root?\n", __LINE__);
      return -1;
    }

    if (!bcm2835_spi_begin())
    {
      dbg_printf(0, "rpi_gpio_init()[%d]: bcm2835_spi_begin failed. Are you running as root?\n", __LINE__);
      bcm2835_close();
      return -1;
    }

    /* Initialize GPIO for AVR reset line
     */
    bcm2835_gpio_fsel(AVR_RESET, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_write(AVR_RESET, HIGH);

    rpi_keyboard_reset();
    sleep(3);

    /* Initialize GPIO for RPi test point and motor-LED
     */
    bcm2835_gpio_fsel(TEST_POINT, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_write(TEST_POINT, LOW);

    bcm2835_gpio_fsel(MOTOR_LED, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_write(MOTOR_LED, HIGH);

    /* Initialize 6-bit DAC, joystick comparator,
     * audio multiplexer control, and emulator reset GPIO lines
     */
    bcm2835_gpio_fsel(DAC_BIT0, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_fsel(DAC_BIT1, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_fsel(DAC_BIT2, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_fsel(DAC_BIT3, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_fsel(DAC_BIT4, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_fsel(DAC_BIT5, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_clr_multi((1 << DAC_BIT0) | (1 << DAC_BIT1) | (1 << DAC_BIT2) |
                           (1 << DAC_BIT3) | (1 << DAC_BIT4) | (1 << DAC_BIT5));

    bcm2835_gpio_fsel(JOYSTK_COMP, BCM2835_GPIO_FSEL_INPT);
    bcm2835_gpio_set_pud(JOYSTK_COMP, BCM2835_GPIO_PUD_OFF);

    bcm2835_gpio_fsel(JOYSTK_BUTTON, BCM2835_GPIO_FSEL_INPT);
    bcm2835_gpio_set_pud(JOYSTK_BUTTON, BCM2835_GPIO_PUD_UP);

    bcm2835_gpio_fsel(AUDIO_MUX0, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_fsel(AUDIO_MUX1, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_clr_multi((1 << AUDIO_MUX0) | (1 << AUDIO_MUX1));

    bcm2835_gpio_fsel(EMULATOR_RESET, BCM2835_GPIO_FSEL_INPT);
    bcm2835_gpio_set_pud(EMULATOR_RESET, BCM2835_GPIO_PUD_UP);

    /* Initialize SPI0 for AVR keyboard interface
     */
    bcm2835_spi_setBitOrder(BCM2835_SPI_BIT_ORDER_MSBFIRST);
    bcm2835_spi_setDataMode(BCM2835_SPI_MODE0);
    bcm2835_spi_setClockDivider(BCM2835_SPI_CLOCK_DIVIDER_128);

    return 0;
}

/********************************************************************
 * rpi_fb_init()
 *
 *  Initialize the RPi frame buffer device.
 *
 *  param:  None
 *  return: Pointer to frame buffer, or 0 if error,
 */
uint8_t *rpi_fb_init(int x_pix, int y_pix)
{
    uint8_t *fbp = 0;

    // Open the frame buffer device file for reading and writing
    if ( fbfd == 0 )
    {
        fbfd = open("/dev/fb0", O_RDWR);
        if (fbfd == -1)
        {
            dbg_printf(0, "rpi_fb_init()[%d]: Cannot open frame buffer /dev/fb0\n", __LINE__);
            return 0;
        }
    }

    dbg_printf(2, "Frame buffer device is open\n");

    if ( (int)(fbp = fb_set_resolution(fbfd, x_pix, y_pix)) == 0 )
    {
        return 0;
    }

    // Select graphics mode to hide cursor
    if ( fb_set_tty(1) )
    {
        dbg_printf(0, "rpi_fb_init()[%d]: Could not set tty0 mode.\n", __LINE__);
        return 0;
    }

    return fbp;
}

/********************************************************************
 * rpi_fb_resolution()
 *
 *  Change the RPi frame buffer resolution.
 *  Frame buffer must be already initialized with rpi_fb_init()
 *
 *  param:  None
 *  return: Pointer to frame buffer, or 0 if error,
 */
/* uint8_t *rpi_fb_resolution(int x_pix, int y_pix)
{
    uint8_t *fbp = 0;

    if ( (int)(fbp = fb_set_resolution(fbfd, x_pix, y_pix)) == 0 )
    {
        return 0;
    }

    return fbp;
} */

/*------------------------------------------------
 * rpi_system_timer()
 *
 *  Return running system timer time stamp
 *
 *  param:  None
 *  return: System timer value
 */
uint32_t rpi_system_timer(void)
{
    return (uint32_t) clock();
}

/*------------------------------------------------
 * rpi_keyboard_read()
 *
 *  Read serial interface from AVR (PS2 keyboard controller)
 *
 *  param:  None
 *  return: Key code
 */
int rpi_keyboard_read(void)
{
    return (int)bcm2835_spi_transfer(0);
}

/*------------------------------------------------
 * rpi_keyboard_reset()
 *
 *  Reset keyboard AVR interface
 *
 *  param:  None
 *  return: None
 */
void rpi_keyboard_reset(void)
{
    bcm2835_gpio_write(AVR_RESET, LOW);
    usleep(10);
    bcm2835_gpio_write(AVR_RESET, HIGH);
}

/*------------------------------------------------
 * rpi_joystk_comp()
 *
 *  Read joystick comparator GPIO input pin and return its value.
 *
 *  param:  None
 *  return: GPIO joystick comparator input level
 */
int rpi_joystk_comp(void)
{
    /* The delay is needed to allow the DAC and comparator
     * to stabilize the output, and propagate it through the
     * 5v/3.3v level-shifter that is bandwidth-limited.
     * The Dragon code is limited by a ~13uSec between writing
     * to DAC and reading comparator input:
     *
     *      STB     PIA1DA          ; send value to D/A converter
     *      TST     PIA0DA          ; read result value, comparator output in bit 7
     *
     * A 20uSec delay seems to stabilize the joystick ADC readings.
     *
     */
    bcm2835_delayMicroseconds(20);

    return (int) bcm2835_gpio_lev(JOYSTK_COMP);
}

/*------------------------------------------------
 * rpi_rjoystk_button()
 *
 *  Read right joystick button GPIO input pin and return its value.
 *
 *  param:  None
 *  return: GPIO joystick button input level
 */
int rpi_rjoystk_button(void)
{
    return (int) bcm2835_gpio_lev(JOYSTK_BUTTON);
}

/*------------------------------------------------
 * rpi_reset_button()
 *
 *  Emulator reset button GPIO input pin and return its value.
 *
 *  param:  None
 *  return: GPIO reset button input level
 */
int rpi_reset_button(void)
{
    return (int) bcm2835_gpio_lev(EMULATOR_RESET);
}

/*------------------------------------------------
 * rpi_audio_mux_set()
 *
 *  Set GPIO to select analog multiplexer output.
 *
 *  param:  Multiplexer select bit field: b.1=PIA1-CB2, b.0=PIA0-CA2
 *  return: None
 */
void rpi_audio_mux_set(int select)
{
    static int previous_select = 0;

    if ( select != previous_select )
    {
        bcm2835_gpio_write_mask((uint32_t)select << AUDIO_MUX0, (uint32_t) AUDIO_MUX_MASK);
        bcm2835_delayMicroseconds(20);  // TODO check if this is needed to reduce value noise
        previous_select = select;
    }
}

/*------------------------------------------------
 * rpi_write_dac()
 *
 *  Write 6-bit value to DAC
 *
 *  param:  DAC value 0x00 to 0x3f
 *  return: None
 */
void rpi_write_dac(int dac_value)
{
    uint32_t    dac_bit_values = 0;

    /* Arrange GPIO bit pin outputs
     */
    dac_bit_values = dac_value << DAC_BIT0;

    /* Set the first 32 GPIO output pins specified in the 'mask' to the value given by 'value'
     *  value: values required for each bit masked in by mask.
     *  mask: of pins to affect
     */
    bcm2835_gpio_write_mask(dac_bit_values, (uint32_t) DAC_BIT_MASK);
}

/*------------------------------------------------
 * rpi_disable()
 *
 *  Disable interrupts
 *
 *  param:  None
 *  return: None
 */
void rpi_disable(void)
{
}

/*------------------------------------------------
 * rpi_enable()
 *
 *  Enable interrupts
 *
 *  param:  None
 *  return: None
 */
void rpi_enable(void)
{
}

/*------------------------------------------------
 * rpi_motor_led_on()
 *
 *  Turn on motor LED indicator.
 *
 *  param:  Source of request disk=1 or tape=2
 *  return: None
 */
void rpi_motor_led_on(uint8_t source)
{
    motor_led_ctrl |= source;
    bcm2835_gpio_write(MOTOR_LED, LOW);
}

/*------------------------------------------------
 * rpi_motor_led_off()
 *
 *  Turn off motor LED indicator.
 *
 *  param:  Source of request disk=1 or tape=2
 *  return: None
 */
void rpi_motor_led_off(uint8_t source)
{
    motor_led_ctrl &= ~source;
    if ( motor_led_ctrl == 0 )
    {
        bcm2835_gpio_write(MOTOR_LED, HIGH);
    }
}

/*------------------------------------------------
 * rpi_testpoint_on()
 *
 *  Set test point to logic '1'
 *
 *  param:  None
 *  return: None
 */
void rpi_testpoint_on(void)
{
    bcm2835_gpio_write(TEST_POINT, HIGH);
}

/*------------------------------------------------
 * rpi_testpoint_off()
 *
 *  Set test point to logic '0'
 *
 *  param:  None
 *  return: None
 */
void rpi_testpoint_off(void)
{
    bcm2835_gpio_write(TEST_POINT, LOW);
}

/********************************************************************
 * rpi_halt()
 *
 *  Output message and halt
 *
 *  param:  Message
 *  return: None
 */
void rpi_halt(void)
{
    dbg_printf(0, "HALT\n");
    assert(0);
}

/*------------------------------------------------
 * _putchar()
 *
 *  Low level character output/stream for printf()
 *
 *  param:  character
 *  return: none
 */
void _putchar(char character)
{
    putchar(character);
}

/********************************************************************
 * fb_set_resolution()
 *
 *  Set screen resolution and return pointer to screen memory buffer.
 *  Function sets frame buffer to 8-bits per-pixel.
 *
 *  param:  Frame buffer device handle, horizontal and vertical resolution
 *          in pixels
 *  return: Pointer to memory, 0L on error
 */
static uint8_t *fb_set_resolution(int fbh, int x_pix, int y_pix)
{
    uint8_t    *fbp = 0;
    int         page_size = 0;
    long int    screen_size = 0;

    struct  fb_var_screeninfo var_info;
    struct  fb_fix_screeninfo fix_info;

    // Get variable screen information
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &var_info))
    {
        dbg_printf(0, "fb_set_resolution()[%d]: Error reading variable screen info.\n", __LINE__);
        return 0L;
    }

    var_info.bits_per_pixel = 8;
    var_info.xres = x_pix;
    var_info.yres = y_pix;
    var_info.xres_virtual = x_pix;
    var_info.yres_virtual = y_pix;
    if ( ioctl(fbfd, FBIOPUT_VSCREENINFO, &var_info) )
    {
        dbg_printf(0, "fb_set_resolution()[%d]: Error setting variable information.\n", __LINE__);
    }

    dbg_printf(2, "Display info: %dx%d, %d bpp\n",
           var_info.xres, var_info.yres,
           var_info.bits_per_pixel);

    // Get fixed screen information
    if ( ioctl(fbfd, FBIOGET_FSCREENINFO, &fix_info) )
    {
        dbg_printf(0, "fb_set_resolution()[%d]: Error reading fixed information.\n", __LINE__);
        return 0L;
    }

    dbg_printf(2, "Device ID: %s\n", fix_info.id);

    // map frame buffer to user memory
    screen_size = var_info.xres * var_info.yres_virtual * var_info.bits_per_pixel / 8;
    page_size = var_info.xres * var_info.yres;

    dbg_printf(2, "Screen_size=%ld, page_size=%d\n", screen_size, page_size);

    if ( screen_size > fix_info.smem_len )
    {
        dbg_printf(0, "fb_set_resolution()[%d]: screen_size over buffer limit.\n", __LINE__);
        return 0L;
    }

    fbp = (uint8_t*)mmap(0,
                         screen_size,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED,
                         fbfd, 0);

    if ( (int)fbp == -1 )
    {
        dbg_printf(0, "fb_set_resolution()[%d]: Failed to mmap()\n", __LINE__);
        return 0;
    }

    return fbp;
}

/********************************************************************
 * fb_set_tty()
 *
 *  Set screen to graphics mode.
 *
 *  param:  tty mode text=0, graphics=1
 *  return: 0 no error
 *         -1 on error
 */
static int fb_set_tty(const int mode)
{
    int     console_fd;
    int     result = 0;

    console_fd = open("/dev/tty0", O_RDWR);

    if ( !console_fd )
    {
        dbg_printf(0, "fb_set_tty()[%d]: Could not open console.\n", __LINE__);
        return -1;
    }

    if ( mode )
    {
        if (ioctl( console_fd, KDSETMODE, KD_GRAPHICS))
        {
            dbg_printf(0, "fb_set_tty()[%d]: Could not set console to KD_GRAPHICS mode.\n", __LINE__);
            result = -1;
        }
    }
    else
    {
        if (ioctl( console_fd, KDSETMODE, KD_TEXT))
        {
            dbg_printf(0, "fb_set_tty()[%d]: Could not set console to KD_TEXT mode.\n", __LINE__);
            result = -1;
        }
    }

    close(console_fd);

    return result;
}
