/********************************************************************
 * dragon.c
 *
 *  DRAGON DATA computer emulator, main module.
 *  With MC6809E CPU emulation.
 *
 *  July 2024
 *
 *******************************************************************/

#include    "trace.h"

#include    "config.h"
#include    "dbgmsg.h"

#include    "mem.h"
#include    "cpu.h"
#include    "rpi.h"
#if (RPI_BARE_METAL==1)
  #include    "rpi-bm/gpio.h"
#endif

#include    "sam.h"
#include    "vdg.h"
#include    "pia.h"
#include    "disk.h"
#include    "tape.h"
#include    "fat32.h"
#include    "loader.h"

/* -----------------------------------------
   Dragon 32 ROM image
----------------------------------------- */
#include    "dragon/dragon.h"
#include    "dragon/ddos10p.h"

/* -----------------------------------------
   Module definition
----------------------------------------- */
#define     IO_TRAP                 0

#define     DRAGON_ROM_START        0x8000
#define     DRAGON_ROM_END          0xfeff
#define     ESCAPE_LOADER           1       // Pressing F1
#define     LONG_RESET_DELAY        1500000 // Micro-seconds to force cold start
#define     VDG_REFRESH_INTERVAL    ((uint32_t)(1000000/50))
#define     HALF_SECOND             ((uint32_t)(500000))

/**** Trace / Breakpoint / IO trap ******/
#if (RPI_BARE_METAL==0)
cpu_state_t     cpu_state;
cpu_run_state_t run_state;
int             breakpoint_trigger = 0;
uint16_t        breakpoint = 0xBE12;        // 'DOSLowLevel' 0xc169 line #4024
uint16_t        io_trap_addr = 0xff20;      // IO trap address
#endif
/****************************************/

/* -----------------------------------------
   Module functions
----------------------------------------- */
static int get_reset_state(uint32_t time);
static uint8_t io_trap(uint16_t address, uint8_t data, mem_operation_t op);

/*------------------------------------------------
 * main()
 *
 */
#if (RPI_BARE_METAL==1)
    void kernel(uint32_t r0, uint32_t machid, uint32_t atags)
#else
    int main(int argc, char *argv[])
#endif
{
    uint32_t    last_refresh_time;
    int         i, no_disk;
    int         emulator_escape_code;

    /* System GPIO initialization
     */
    if ( rpi_gpio_init() == -1 )
    {
        dbg_printf(0, "GPIO failed to initialize. Halting\n");
        rpi_halt();
    }
    else
    {
        dbg_printf(2, "GPIO initialized.\n");
    }
    
    if ( (i = fat32_init()) != NO_ERROR )
    {
        dbg_printf(0, "FAT32 initialization failed (%d).\n", i);
    }
    else
    {
        dbg_printf(2, "FAT32 on SD initialized.\n");
    }
    
    dbg_printf(0, "Dragon 32 %s %s\n", __DATE__, __TIME__);
    dbg_printf(0, "Debug level = %d\n", DEBUG_LVL);

    /* Emulation initialization
     */
    dbg_printf(1, "Initializing peripherals.\n");
    loader_init();
    sam_init();
    pia_init();
    vdg_init();
    tape_init();

    /* If joystick button is pressed during bootup
     * then don't install disk support.
     */
    no_disk = 0;
    if ( rpi_rjoystk_button() == 0 )
    {
        last_refresh_time = rpi_system_timer();
        while ( (rpi_system_timer() - last_refresh_time) < HALF_SECOND );
        if ( rpi_rjoystk_button() == 0 )
            no_disk = 1;
    }

    /* ROM code load
     */
    dbg_printf(1, "Loading ROM.\n");

    mem_load(LOAD_ADDRESS, code, sizeof(code));
    dbg_printf(2, "  Loaded Dragon 32, %i bytes.\n", sizeof(code));

    if ( !no_disk )
    {
        disk_init();
        mem_load(DDOS_LOAD_ADDRESS, ddos10p_code, sizeof(ddos10p_code));
        dbg_printf(2, "  Loaded Dragon DOS 1.0p, %i bytes.\n", sizeof(ddos10p_code));
    }

    mem_define_rom(DRAGON_ROM_START, DRAGON_ROM_END);

    /*************** IO trap ****************/
#if (RPI_BARE_METAL==0 && IO_TRAP==1)

    /* NOTE: this trap will override any IO handers set
     * previousely. This might break emulation functionality!
     */
    mem_define_io(io_trap_addr, io_trap_addr, io_trap);

#endif
    /****************************************/

    dbg_printf(2, "Initializing CPU.\n");
    cpu_init(RUN_ADDRESS);

    /* CPU endless execution loop.
     */
    dbg_printf(1, "Starting CPU.\n");
    cpu_reset(1);

    last_refresh_time = rpi_system_timer();

    for (;;)
    {
        //rpi_testpoint_on();
        cpu_run();
        //rpi_testpoint_off();

        switch ( get_reset_state(LONG_RESET_DELAY) )
        {
            case 0:
                cpu_reset(0);
                break;

            case 2:
                /* Cold start flag set to value that is not 0x55
                 */
                mem_write(0x71, 0);
                dbg_printf(1, "Force cold restart.\n");
                /* no break */

            case 1:
                cpu_reset(1);
                break;

            default:
                dbg_printf(1, "Unknown reset state.\n");
        }

        disk_io_interrupt();

        emulator_escape_code = pia_function_key();
        if ( emulator_escape_code == ESCAPE_LOADER )
            loader();

        if ( (rpi_system_timer() - last_refresh_time) >= VDG_REFRESH_INTERVAL )
        {
            last_refresh_time = rpi_system_timer();
            //rpi_testpoint_on();
            vdg_render();
            //rpi_testpoint_off();
            pia_vsync_irq();
        }
        
        /********** Trace / Breakpoint **********/
#if (RPI_BARE_METAL==0)

        run_state = cpu_get_state(&cpu_state);

        if ( cpu_state.cpu_state == CPU_EXCEPTION )
        {
            dbg_printf(0, "Op-code Exception at pc=0x%04x last_pc=0x%04x\n", cpu_state.pc, cpu_state.last_pc);
            breakpoint_trigger = 1;
        }

        //if ( cpu_state.pc == breakpoint )
        if ( 0 )
        {
            breakpoint_trigger = 1;
        }

        if ( breakpoint_trigger )
        {
            trace_print_registers(&cpu_state);
            breakpoint_trigger = trace_action(&breakpoint);
        }
        
#endif
        /****************************************/
    }

#if (RPI_BARE_METAL==0)
    return 0;
#endif
}

/*------------------------------------------------
 * get_reset_state()
 *
 * Scan the reset button with rpi_reset_button() and return
 * '1' for short reset and '2' for long reset press.
 * '0' for no press.
 * Accepts 'time' in micro-seconds as a parameter for determining long
 * press.
 *
 * param:  minimum button press time in micro-seconds
 * return: '0'=no press, '1'=short reset, '2'=long reset press.
 *
 */
static int get_reset_state(uint32_t time)
{
    uint32_t    start_time;
    int         reset_type = 0;

    if ( rpi_reset_button() == 0 )  // Active low!
    {
        start_time = rpi_system_timer();
        while ( !rpi_reset_button() );
        if ( (rpi_system_timer() - start_time) >= time )
        {
            reset_type = 2;
        }
        else
        {
            reset_type = 1;
        }
    }

    return reset_type;
}

/*------------------------------------------------
 * io_trap()
 *
 * Output IO trap details to stdout.
 *
 *  param:  Call address, data byte for write operation, and operation type
 *  return: Status or data byte
 *
 */
static uint8_t io_trap(uint16_t address, uint8_t data, mem_operation_t op)
{
#if (RPI_BARE_METAL==0)
    cpu_get_state(&cpu_state);
    dbg_printf(0, "io_trap(): io address=0x%04x data=0x%02x (%c) pc=0x%04x last pc=0x%04x\n",
                              address, data, (op ? 'W' : 'R'), cpu_state.pc, cpu_state.last_pc);
#endif

    return data;
}