/********************************************************************
 * rpibm.c
 *
 *  Functions and definitions for RPi machine-dependent functionality.
 *  This is the bare metal implementation.
 *
 *  July 2024
 *
 *******************************************************************/

#include    <string.h>

#include    "rpi-bm/bcm2835.h"
#include    "rpi-bm/gpio.h"
#include    "rpi-bm/auxuart.h"
#include    "rpi-bm/timer.h"
#include    "rpi-bm/spi0.h"
#include    "rpi-bm/spi1.h"
#include    "rpi-bm/mailbox.h"
#include    "rpi-bm/irq.h"
#include    "printf.h"
#include    "rpi.h"

/* -----------------------------------------
   Local definitions
----------------------------------------- */
// AVR and keyboard
#define     AVR_RESET           RPI_V2_GPIO_P1_11
#define     PRI_TEST_POINT      RPI_V2_GPIO_P1_07

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

typedef struct
    {
        int yoffset;    // Current offset into virtual buffer
        int pitch;      // Bytes per display line
        int xres;       // X pixels
        int yres;       // Y pixels
    } var_info_t;

/* -----------------------------------------
   Module static functions
----------------------------------------- */

/* -----------------------------------------
   Module globals
----------------------------------------- */
static var_info_t   var_info;

/* Palette for 8-bpp color depth.
 * The palette is in BGR format, and 'set pixel order' does not affect
 * palette behavior.
 * Palette source: https://www.rapidtables.com/web/color/RGB_Color.html
 *                 https://en.wikipedia.org/wiki/Web_colors#HTML_color_names
 */
static uint32_t     palette_bgr[] =
{
        0x00000000,
        0x00800000,
        0x00008000,
        0x00808000,
        0x00000080,
        0x00800080,
        0x0000a5ff,
        0x00C0C0C0,
        0x00808080,
        0x00FF0000,
        0x0000FF00,
        0x00FFFF00,
        0x000000FF,
        0x00FF00FF,
        0x0000FFFF,
        0x00FFFFFF
};

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
    /* Initialize auxiliary UART for console output.
     * Safe to continue with system bring-up even if UART failed?
     */
    bcm2835_auxuart_init(DEFAULT_UART_RATE, 100, 100, AUXUART_DEFAULT);

    /* Initialize SPI0 for AVR keyboard interface
     */
    if ( !bcm2835_spi0_init(SPI0_DEFAULT) )
    {
      printf("rpi_gpio_init(): bcm2835_spi_init() failed.\n");
      return -1;
    }

    bcm2835_spi0_set_rate(DEFAULT_SPI0_RATE);

    /* Initialize GPIO for AVR reset line
     */
    bcm2835_gpio_fsel(AVR_RESET, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_set(AVR_RESET);

    rpi_keyboard_reset();
    bcm2835_st_delay(3000000);

    /* Initialize GPIO for RPi test point
     */
    bcm2835_gpio_fsel(PRI_TEST_POINT, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_clr(PRI_TEST_POINT);

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
    static mailbox_tag_property_t *mp;

    uint8_t *fbp = 0;
    int      page_size = 0;
    long int screen_size = 0;

    bcm2835_mailbox_init();
    bcm2835_mailbox_add_tag(TAG_FB_ALLOCATE, 4);
    bcm2835_mailbox_add_tag(TAG_FB_SET_PHYS_DISPLAY, x_pix, y_pix);
    bcm2835_mailbox_add_tag(TAG_FB_SET_VIRT_DISPLAY, x_pix, y_pix);
    bcm2835_mailbox_add_tag(TAG_FB_SET_DEPTH, 8);
    bcm2835_mailbox_add_tag(TAG_FB_SET_PALETTE, 0, 16, (uint32_t)palette_bgr);
    bcm2835_mailbox_add_tag(TAG_FB_GET_PITCH);
    if ( !bcm2835_mailbox_process() )
    {
        printf("rpi_fb_init(): bcm2835_mailbox_process() failed.\n");
        return 0;
    }

    /* Get fixed screen information
     */
    mp = bcm2835_mailbox_get_property(TAG_FB_ALLOCATE);
    if ( mp )
    {
        screen_size = mp->values.fb_alloc.param2;
        fbp = (uint8_t*)(mp->values.fb_alloc.param1);
    }
    else
    {
        printf("rpi_fb_init(): TAG_FB_ALLOCATE failed.\n");
        return 0;
    }

    mp = bcm2835_mailbox_get_property(TAG_FB_SET_PHYS_DISPLAY);
    if ( mp &&
         mp->values.fb_set.param1 == x_pix &&
         mp->values.fb_set.param2 == y_pix )
    {
        page_size = x_pix * y_pix;
        var_info.xres = x_pix;
        var_info.yres = y_pix;
        var_info.yoffset = 0;
    }
    else
    {
        printf("rpi_fb_init(): TAG_FB_SET_PHYS_DISPLAY failed.\n");
        return 0;
    }

    mp = bcm2835_mailbox_get_property(TAG_FB_GET_PITCH);
    if ( mp )
    {
        var_info.pitch = mp->values.fb_get.param1;
    }
    else
    {
        printf("rpi_fb_init(): TAG_FB_GET_PITCH failed\n");
        return 0;
    }

    printf("Frame buffer device is open:\n");
    printf("  x_pix=%d, y_pix=%d, screen_size=%d, page_size=%d\n",
                       x_pix, y_pix, screen_size, page_size);

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
uint8_t *rpi_fb_resolution(int x_pix, int y_pix)
{
    uint8_t *fbp = 0;

    if ( (int)(fbp = rpi_fb_init(x_pix, y_pix)) == 0 )
    {
        return 0;
    }

    return fbp;
}

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
    return (uint32_t) bcm2835_st_read();
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
    return (int)bcm2835_spi0_transfer_byte(0);
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
    bcm2835_gpio_clr(AVR_RESET);
    bcm2835_st_delay(10);
    bcm2835_gpio_set(AVR_RESET);
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
     * TODO: Try bcm2835_st_delay()?
     *
     */
    bcm2835_crude_delay(20);

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
        bcm2835_crude_delay(20);    // TODO check if this is needed to reduce value noise
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
    disable();
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
    enable();
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
    bcm2835_gpio_set(PRI_TEST_POINT);
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
    bcm2835_gpio_clr(PRI_TEST_POINT);
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
    printf("HALT\n");
    for (;;)
    {
    }
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
    if ( character == '\n')
        bcm2835_auxuart_putchr('\r');
    bcm2835_auxuart_putchr(character);
}
