/********************************************************************
 * disk.c
 *
 *  This module defines disk cartridge function including
 *  controller IC WD2797, drive and motor control register, and interrupts
 * 
 *  resources:
 *  WD2797 floppy disk controller data sheet
 *  Dragon DOS programmer's guide, Grosvenor Software 1985
 *  Dragon DOS cartridge schematics
 *  Dragon DOS source code and ROM iamges https://github.com/prime6809/DragonDOS
 *  https://worldofdragon.org/index.php?title=Tape%5CDisk_Preservation#JVC.2FDSK_File_Format
 *
 *  July 2024
 *
 *******************************************************************/

#include    <stdint.h>
#include    <string.h>

#include    "fat32.h"
#include    "disk.h"
#include    "loader.h"
#include    "cpu.h"
#include    "mem.h"
#include    "pia.h"
#include    "rpi.h"

#include    "dbgmsg.h"

/* -----------------------------------------
   Local definitions
----------------------------------------- */

/* WD2797 Floppy disk controler (IC7)
 */
#define     WDCmdReg            0xff40      // command/status         
#define     WDTrkReg            0xff41      // Track register
#define     WDSecReg            0xff42      // Sector register
#define     WDDataReg           0xff43      // Data register

/* Control register
 */
#define     DskCtl              0xff48      // Disk DS/motor control reg

/* Disk command codes WD2797, Dragon Dos
 * Only a subset of commands are used, all others will be treated as illigal commands.
 * Note: mask bit.0 and bit.1 for Type 1 commands Seek and Restore. 
 */
#define     WDCmdRestore        0b00000000  // 0x00 Restore to track 0 (unload head, no verify)
#define     WDCmdSeek           0b00010000  // 0x10 Seek to track (unload head, no verify)
#define     WDCmdReadSec        0b10001000  // 0x88 Read sector command (single record, SSO to 0)
#define     WDCmdWriteSec       0b10101000  // 0xA8 Write sector command (single record, SSO to 0)
#define     WDCmdReadAddr       0b11000000  // 0xC0 Read address mark
#define     WDCmdForceInt       0b11010000  // 0xD0 Force inturrupt (terminate with no interrupt)
#define     WDCmdWriteTrack     0b11110100  // 0xF4 Write (format) track (SSO to 0)
#define     WDCmdStepMask       0b11111100  // Step rate mask

/* WD2797 Error flag and status bits
 */
#define     WDNotReady          0b10000000  // Drive not ready
#define     WDWriteProt         0b01000000  // Write protect
#define     WDHeadLoaded        0b00100000  // Head loaded, type 1
#define     WDRecType           0b00100000  // Record type, read sec
#define     WDSeek              0b00010000  // Seek error, type 1
#define     WDRNF               0b00010000  // Record not found, read address, sector write sector
#define     WDCRC               0b00001000  // CRC error all but read/write track
#define     WDTrack0            0b00000100  // Head on track 0, type 1
#define     WDLostData          0b00000100  // Lost data 
#define     WDIndex             0b00000010  // Index pulse, type 1
#define     WDDRQ               0b00000010  // Data request
#define     WDBusy              0x00000001  // Busy

/* Disk control register bitmasks for Disk and motor control (IC1 & IC2)
 */
#define     NMIE                0b00100000  // NMI enable/disable (IC3)
#define     WPCE                0b00010000  // Write pre-compensation, WD2797 pin.1 ENP
#define     SDensE              0b00001000  // Single density enable, WD2797 pin.37 DDEN
#define     MotorOn             0b00000100  // Motor on/off
#define     Drive0              0b00000000  // Drive select 0 (IC1 & IC2)
#define     Drive1              0b00000001  // Drive select 1 (IC1 & IC2)
#define     Drive2              0b00000010  // Drive select 2 (IC1 & IC2)
#define     Drive3              0b00000011  // Drive select 3 (IC1 & IC2)
#define     DriveMask           0b00000011  // Mask to extract drives

/* Diskette geometry
 * Single-side, normal density.
 */
#define     TRACK_PER_DISK      40          // 0 to 39
#define     SEC_PER_TRACK       18          // 1 to 18
#define     SECTOR_SIZE         256         // Bytes
#define     BYTES_PER_TRACK     (SEC_PER_TRACK * SECTOR_SIZE)
#define     ID_FIELD_SIZE       6           // Bytes
#define     FILE_VDK_HEADER     12          // Bytes
#define     DISK_INT_INTERVAL   1000        // uSec

#define     INIT_SEC_FILL       0xe5        // Sector data initialization data
#define     INIT_BYTE_SKIP      111         // Bytes to skip in track init byte stream.

/* -----------------------------------------
   Module types
----------------------------------------- */
typedef enum
{
    DISK_UNDEFINED  = -1,
    DISK_IDLE       = 0,    // Waiting for command
    DISK_READ       = 1,    // Reading
    DISK_WRITE      = 2,    // Writeing
    DISK_READ_ID    = 3,    // Read 6-byte disk ID/location (see 'WDCmdReadAddr')
    DISK_WRITE_TRK  = 4,    // Write track / format
} disk_state_t;

typedef struct
{
    uint8_t     char_d;
    uint8_t     char_k;
    uint16_t    header_size;
    uint8_t     vdk_version;
    uint8_t     vdk_version_old;
    uint8_t     source_id;
    uint8_t     source_version;
    uint8_t     tracks;
    uint8_t     sides;
    uint8_t     flags;
    uint8_t     compression;
} disk_vdk_header_t;

typedef struct
{
    uint8_t     track;
    uint8_t     head;
    uint8_t     sector;
    uint8_t     size;
} disk_init_sector_id;

/* -----------------------------------------
   Module static functions
----------------------------------------- */
static uint8_t  io_handler_wd2797_cmd_stat(uint16_t address, uint8_t data, mem_operation_t op);
static uint8_t  io_handler_wd2797_track(uint16_t address, uint8_t data, mem_operation_t op);
static uint8_t  io_handler_wd2797_sector(uint16_t address, uint8_t data, mem_operation_t op);
static uint8_t  io_handler_wd2797_data(uint16_t address, uint8_t data, mem_operation_t op);
static uint8_t  io_handler_drive_ctrl(uint16_t address, uint8_t data, mem_operation_t op);

static uint32_t disk_to_image_offset(uint16_t track, uint16_t sector);
static void     disk_intrq(void);

/* -----------------------------------------
   Module globals
----------------------------------------- */
static int                  nmi_inhibit;
static disk_state_t         state;
static disk_vdk_header_t    vdk_header;

static uint8_t              buffer[BYTES_PER_TRACK];
static int                  buffer_index;

static struct   disk_reg_t
{
    uint8_t disk_cmd;
    uint8_t disk_status;
    uint8_t disk_track;
    uint8_t disk_sector;
    uint8_t disk_data;
    int     motor_on;
    int     disk_drive_num;
    int     disk_double_density;
} disk_registers;

/*------------------------------------------------
 * disk_init()
 *
 *  Initialize the disk subsystem
 *
 *  param:  Nothing
 *  return: Nothing
 */
void disk_init(void)
{
    mem_define_io(WDCmdReg, WDCmdReg, io_handler_wd2797_cmd_stat);
    mem_define_io(WDTrkReg, WDTrkReg, io_handler_wd2797_track);
    mem_define_io(WDSecReg, WDSecReg, io_handler_wd2797_sector);
    mem_define_io(WDDataReg, WDDataReg, io_handler_wd2797_data);
    mem_define_io(DskCtl, DskCtl, io_handler_drive_ctrl);

    disk_registers.disk_cmd = WDCmdRestore;
    disk_registers.disk_status = WDTrack0;
    disk_registers.disk_track = 0;
    disk_registers.disk_sector = 1;
    disk_registers.disk_data = 0;
    disk_registers.motor_on = 0;
    disk_registers.disk_drive_num = 0;
    disk_registers.disk_double_density = 0;

    nmi_inhibit = 1;
    state = DISK_IDLE;
}

/*------------------------------------------------
 * disk_io_interrupt()
 *
 *  Simulate interrupts from WD2797 that is ready for read or write.
 *  Call periodically from the main emulation loop.
 *
 *  param:  Nothing
 *  return: Nothing
 */
void disk_io_interrupt(void)
{
    static uint32_t time_mark = 0;

    uint32_t        duration;

    /* The routine has a state machine with two states indicated
     * by 'time_mark' as ==0 or !=0.
     * In normal operation 'time_mark' is 0 and interrupts are not generator.
     * When data is available for read or when the emulation is ready to write,
     * a 'state' not DISK_IDLE triggers a transition to a state in which
     * this routine generates FIRQ interrupts through PIA1.
     * When the 'state' transitions back to DISK_IDLE after all bytes have been
     * read or written, the routine generates and NMI. 
     */

    if ( state != DISK_IDLE && time_mark == 0 )
    {
        time_mark = rpi_system_timer();
    }

    /* This creates a delay between interrupts to compensate for emulation
     * vs. code timing race conditions. The NMI interrupt is generated after a longer delay.
     */
    duration = (state == DISK_IDLE) ? (250*DISK_INT_INTERVAL) : DISK_INT_INTERVAL;

    if ( time_mark && ((rpi_system_timer() - time_mark) >= duration) )
    {
        if ( state == DISK_READ ||
             state == DISK_WRITE ||
             state == DISK_READ_ID ||
             state == DISK_WRITE_TRK )
        {
            time_mark = rpi_system_timer();
            disk_registers.disk_status |= WDDRQ;
            pia_cart_firq();
        }
        else if ( state == DISK_IDLE )
        {
            time_mark = 0;
            disk_intrq();
        }
        else
        {
            dbg_printf(0, "disk_io_interrupt()[%3d]: unhandled state %d.\n", __LINE__, state);
            rpi_halt();
        }
    }
}

/*------------------------------------------------
 * io_handler_wd2797_cmd_stat()
 *
 *  IO call-back handler disk controller WD2797 command/status register
 *
 *  param:  Call address, data byte for write operation, and operation type
 *  return: Status or data byte
 */
static uint8_t io_handler_wd2797_cmd_stat(uint16_t address, uint8_t data, mem_operation_t op)
{
    uint8_t     response = 0;
    uint32_t    seek_address;

    if ( op == MEM_WRITE )
    {
        disk_registers.disk_cmd = data;

        if ( data == WDCmdForceInt )
        {
            dbg_printf(2, "io_handler_wd2797_cmd_stat()[%3d]: WDCmdForceInt\n", __LINE__);
            state = DISK_IDLE;

            disk_registers.disk_status = 0;
        }
        else if ( !(disk_registers.disk_status & WDBusy) )
        {
            if ( (data & WDCmdStepMask) == WDCmdRestore )
            {
                dbg_printf(2, "io_handler_wd2797_cmd_stat()[%3d]: WDCmdRestore\n", __LINE__);
                state = DISK_IDLE;

                disk_registers.disk_track = 0;
                disk_registers.disk_status = WDTrack0;

                disk_intrq();
            }
            else if ( (data & WDCmdStepMask) == WDCmdSeek )
            {
                dbg_printf(2, "io_handler_wd2797_cmd_stat()[%3d]: WDCmdSeek\n", __LINE__);
                state = DISK_IDLE;

                disk_registers.disk_track = disk_registers.disk_data;
                if ( disk_registers.disk_track == 0 )
                    disk_registers.disk_status = WDTrack0;
                else
                    disk_registers.disk_status = 0;

                disk_intrq();
            }
            else if ( data == WDCmdReadSec )
            {
                dbg_printf(2, "io_handler_wd2797_cmd_stat()[%3d]: WDCmdReadSec\n", __LINE__);
                if ( loader_disk_img_type() != FILE_NONE )
                {
                    state = DISK_READ;

                    seek_address = disk_to_image_offset(disk_registers.disk_track, disk_registers.disk_sector);
                    if ( loader_disk_img_type() == FILE_VDK )
                    {
                        loader_disk_fseek(0);
                        loader_disk_fread((uint8_t*) &vdk_header, sizeof(disk_vdk_header_t));
                        seek_address += vdk_header.header_size;
                        dbg_printf(2, "                                   Disk: sides=%d, tracks=%d\n",
                                   vdk_header.sides, vdk_header.tracks);
                    }

                    dbg_printf(2, "                                   Seek=0x%08x, track=%d, sec=%d\n",
                               seek_address, disk_registers.disk_track, disk_registers.disk_sector);
                    
                    loader_disk_fseek(seek_address);
                    loader_disk_fread(buffer, SECTOR_SIZE);

                    disk_registers.disk_status = WDBusy;

                    buffer_index = 0;
                }
            }
            else if ( data == WDCmdWriteSec )
            {
                dbg_printf(2, "io_handler_wd2797_cmd_stat()[%3d]: WDCmdWriteSec\n", __LINE__);
                if ( loader_disk_img_type() != FILE_NONE )
                {
                    state = DISK_WRITE;

                    seek_address = disk_to_image_offset(disk_registers.disk_track, disk_registers.disk_sector);
                    if ( loader_disk_img_type() == FILE_VDK )
                    {
                        loader_disk_fseek(0);
                        loader_disk_fread((uint8_t*) &vdk_header, sizeof(disk_vdk_header_t));
                        seek_address += vdk_header.header_size;
                        dbg_printf(2, "                                   Disk: sides=%d, tracks=%d\n",
                                   vdk_header.sides, vdk_header.tracks);
                    }

                    dbg_printf(2, "                                   Seek=0x%08x, track=%d, sec=%d\n",
                               seek_address, disk_registers.disk_track, disk_registers.disk_sector);
                    
                    loader_disk_fseek(seek_address);

                    disk_registers.disk_status = WDBusy;

                    buffer_index = 0;
                }
            }
            else if ( data == WDCmdReadAddr )
            {
                dbg_printf(2, "io_handler_wd2797_cmd_stat()[%3d]: WDCmdReadAddr\n", __LINE__);
                state = DISK_READ_ID;

                buffer[0] = disk_registers.disk_track;
                buffer[1] = 1;
                buffer[2] = disk_registers.disk_sector;
                buffer[3] = (SECTOR_SIZE - 1);
                buffer[4] = 0xbe;
                buffer[5] = 0xef;

                disk_registers.disk_status = WDBusy;

                buffer_index = 0;
            }
            else if ( data == WDCmdWriteTrack)
            {
                dbg_printf(2, "io_handler_wd2797_cmd_stat()[%3d]: WDCmdWriteTrack\n", __LINE__);
                if ( loader_disk_img_type() != FILE_NONE )
                {
                    state = DISK_WRITE_TRK;

                    seek_address = disk_to_image_offset(disk_registers.disk_track, disk_registers.disk_sector);
                    if ( loader_disk_img_type() == FILE_VDK )
                    {
                        loader_disk_fseek(0);
                        loader_disk_fread((uint8_t*) &vdk_header, sizeof(disk_vdk_header_t));
                        seek_address += vdk_header.header_size;
                        dbg_printf(2, "                                   Disk: sides=%d, tracks=%d\n",
                                vdk_header.sides, vdk_header.tracks);
                    }

                    disk_registers.disk_status = WDBusy;

                    buffer_index = 0;
                }
            }
            else
            {
                state = DISK_IDLE;
                dbg_printf(1, "io_handler_wd2797_cmd_stat()[%3d]: WD2797 bad command 0x%02x.\n", __LINE__, data);
                rpi_halt();
            }
        }
    }
    else // op == MEM_READ
    {
        response = disk_registers.disk_status;
    }

    return response;
}

/*------------------------------------------------
 * io_handler_wd2797_track()
 *
 *  IO call-back handler disk controller WD2797 track register
 *
 *  param:  Call address, data byte for write operation, and operation type
 *  return: Status or data byte
 */
static uint8_t io_handler_wd2797_track(uint16_t address, uint8_t data, mem_operation_t op)
{
    uint8_t     response = 0;

    if ( !(disk_registers.disk_status && WDBusy) && op == MEM_WRITE )
    {
        disk_registers.disk_track = data;
    }
    else
    {
        /* If the controller is busy, the data in this address location
         * will be the existing track and not the one we're trying to write.
         */
        response = disk_registers.disk_track;
    }
    
    return response;
}

/*------------------------------------------------
 * io_handler_wd2797_sector()
 *
 *  IO call-back handler disk controller WD2797 sector register
 *
 *  param:  Call address, data byte for write operation, and operation type
 *  return: Status or data byte
 */
static uint8_t io_handler_wd2797_sector(uint16_t address, uint8_t data, mem_operation_t op)
{
    uint8_t     response = 0;

    if ( !(disk_registers.disk_status && WDBusy) && op == MEM_WRITE )
    {
        disk_registers.disk_sector = data;
    }
    else
    {
        /* If the controller is busy, the data in this address location
         * will be the existing sector and not the one we're trying to write.
         */
        response = disk_registers.disk_sector;
    }

    return response;
}

/*------------------------------------------------
 * io_handler_wd2797()
 *
 *  IO call-back handler disk controller WD2797 data register
 *
 *  param:  Call address, data byte for write operation, and operation type
 *  return: Status or data byte
 */
static uint8_t  io_handler_wd2797_data(uint16_t address, uint8_t data, mem_operation_t op)
{
    uint8_t     response = 0;
    uint32_t    seek_address;

    switch ( state )
    {
        case DISK_IDLE:
            if ( op == MEM_READ )
                response = disk_registers.disk_data;
            else // op == MEM_WRITE
                disk_registers.disk_data = data;
            break;

        case DISK_READ:
            if ( op == MEM_READ )
            {
                disk_registers.disk_data = buffer[buffer_index];
                response = disk_registers.disk_data;
                disk_registers.disk_status &= ~WDDRQ;

                buffer_index++;
                if ( buffer_index == SECTOR_SIZE )
                {
                    state = DISK_IDLE;
                    disk_registers.disk_status &= ~WDBusy;
                }
            }
            break;

        case DISK_WRITE:
            if ( op == MEM_WRITE )
            {
                disk_registers.disk_data = data;
                buffer[buffer_index] = data;
                disk_registers.disk_status &= ~WDDRQ;

                buffer_index++;
                if ( buffer_index == SECTOR_SIZE )
                {
                    state = DISK_IDLE;
                    loader_disk_fwrite(buffer, SECTOR_SIZE);
                    disk_registers.disk_status &= ~WDBusy;
                }
            }
            break;
        
        case DISK_READ_ID:
            if ( op == MEM_READ )
            {
                disk_registers.disk_data = buffer[buffer_index];
                response = disk_registers.disk_data;
                disk_registers.disk_status &= ~WDDRQ;

                buffer_index++;
                if ( buffer_index == ID_FIELD_SIZE )
                {
                    state = DISK_IDLE;
                    disk_registers.disk_status &= ~(WDBusy + WDDRQ);
                }
            }
            break;

        case DISK_WRITE_TRK:
        if ( op == MEM_WRITE )
            {
                disk_registers.disk_data = data;
                disk_registers.disk_status &= ~WDDRQ;

                if ( buffer_index < INIT_BYTE_SKIP )
                {
                    buffer_index++;
                }
                else if ( buffer_index < (INIT_BYTE_SKIP + sizeof(disk_init_sector_id)) )
                {
                    buffer[buffer_index] = data;
                    buffer_index++;
                }
                else
                {
                    state = DISK_IDLE;
                    seek_address = disk_to_image_offset(buffer[INIT_BYTE_SKIP], 1);
                    seek_address += vdk_header.header_size;
                    dbg_printf(2, "                                   Writing track %d (0x%08x).\n", buffer[INIT_BYTE_SKIP], seek_address);

                    memset(buffer, INIT_SEC_FILL, BYTES_PER_TRACK);
                    loader_disk_fseek(seek_address);
                    loader_disk_fwrite(buffer, BYTES_PER_TRACK);
                    disk_registers.disk_status &= ~WDBusy;
                }                   
            }
            break;

        default:
            dbg_printf(0, "io_handler_wd2797_data()[%3d]: unhandled state %d.\n", __LINE__, state);
            rpi_halt();
            break;
    }

    return response;
}

/*------------------------------------------------
 * io_handler_drive_ctrl()
 *
 *  IO call-back handler for disk drive and motor control register/IO-port.
 *  The call-back handles and updates drive state/mode parameters.
 *
 *  param:  Call address, data byte for write operation, and operation type
 *  return: Status or data byte
 */
static uint8_t io_handler_drive_ctrl(uint16_t address, uint8_t data, mem_operation_t op)
{
    int     new_drive_num;

    new_drive_num = (int) (data & DriveMask);
    nmi_inhibit = (data & NMIE) ? 0 : 1;
    disk_registers.motor_on = (data & MotorOn) ? 1 : 0;
    disk_registers.disk_double_density = (data & SDensE) ? 1 : 0;

    /* Handle drive change
     */
    if ( new_drive_num != disk_registers.disk_drive_num )
    {
        dbg_printf(0, "io_handler_drive_ctrl()[%3d]: drive change to %d.\n", __LINE__, new_drive_num);
        rpi_halt();
    }
    
    /* Handle motor on-off state change
     */
    if ( disk_registers.motor_on )
    {
        rpi_motor_led_on(MOTOR_LED_DISK);
    }
    else
    {
        rpi_motor_led_off(MOTOR_LED_DISK);
    }

    dbg_printf(2, "io_handler_drive_ctrl()[%3d]: data = 0x%02x\n"   \
                  "                              Drive:       %d\n" \
                  "                              NMI_INH:     %d\n" \
                  "                              Motor-on:    %d\n" \
                  "                              Double dens: %d\n" \
                  "                              Wr Pre-comp: %d\n",
               __LINE__, data, disk_registers.disk_drive_num, nmi_inhibit,
               disk_registers.motor_on, disk_registers.disk_double_density, (data & WPCE) ? 0 : 1);

    return data;
}

/*------------------------------------------------
 * disk_to_image_offset()
 *
 *  Canculate data offset into single-side disk image file.
 *  The image file offset does not include optional header size.
 *
 *  param:  Track and Sector
 *  return: Image file offset
 */
static uint32_t disk_to_image_offset(uint16_t track, uint16_t sector)
{
    return (uint32_t) (SECTOR_SIZE * (SEC_PER_TRACK * track + (sector - 1)));
}

/*------------------------------------------------
 * disk_intrq()
 *
 *  Trigger an interrupt request on the NMI line.
 *
 *  param:  None
 *  return: None
 */
static void disk_intrq(void)
{
    if ( !nmi_inhibit )
    {
        cpu_nmi_trigger();
    }
}