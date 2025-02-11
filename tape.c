/********************************************************************
 * tape.c
 *
 *  This module defines cassette tape write function.
 * 
 *  Resources:
 *   CAS file format https://worldofdragon.org/index.php?title=Tape%5CDisk_Preservation 
 *   Dragon ROM https://github.com/prime6809/DragonRom
 * 
 *  December 2024
 *
 *******************************************************************/

#include    <stdint.h>
#include    <ctype.h>
#include    <string.h>

#include    "loader.h"
#include    "cpu.h"
#include    "mem.h"

#include    "dbgmsg.h"

/* -----------------------------------------
   Local definitions
----------------------------------------- */
#define     CAS_STREAM_SIZE         (64*1024)

/* 'CasLastSine' addresses are different becase of different DP
 * register values at entry to 'CasByteOut' routine when using
 * BASIC vs All Dream assembler. Is it a bug in All Dream?
 * 
 * It is risky registering the All Dream addresses becsaue these are
 * general RAM addresses that can be used by other programs when All Drean is
 * not loaded. Maybe add a hack to Dragon ROM?
 */
#define     CASLASTSINE_BASIC       0x0085      // 'CasLastSine'
#define     CASLASTSINE_AD_ROM      0x7e85      // 'CasLastSine' All-Dream ROM
#define     CASLASTSINE_AD_CAS      0x5f85      // 'CasLastSine' All-Dream cassette tape

/* -----------------------------------------
   Module types
----------------------------------------- */
typedef enum
{
    CAS_STREAM_IDLE = 0,
    CAS_STREAM_LEADER,
    CAS_STREAM_SYNC,
    CAS_STREAM_HEADER,
    CAS_STREAM_DATA,
    CAS_STREAM_EOF,
    CAS_STREAM_WRITE,
} cas_tape_state_t;

/* -----------------------------------------
   Module static functions
----------------------------------------- */
static uint8_t io_handler_cas_tape(uint16_t address, uint8_t data, mem_operation_t op);
static cas_tape_state_t cas_stream_header(uint8_t data_byte, char* file_name);
static cas_tape_state_t cas_stream_data(uint8_t data_byte);
static cas_tape_state_t cas_stream_eof(uint8_t data_byte);

/* -----------------------------------------
   Module globals
----------------------------------------- */
static uint8_t      cas_stream_buffer[CAS_STREAM_SIZE];

/*------------------------------------------------
 * tape_init()
 *
 *  Initialize the tape subsystem
 *
 *  param:  Nothing
 *  return: Nothing
 */
void tape_init(void)
{
    mem_define_io(CASLASTSINE_BASIC, CASLASTSINE_BASIC, io_handler_cas_tape);

    /* It is risky registering the All Dream addresses for ROM and tape becsaue these are
     * general RAM addresses that can be used by other programs when All Drean is
     * not loaded.
     */
    mem_define_io(CASLASTSINE_AD_ROM, CASLASTSINE_AD_ROM, io_handler_cas_tape); // *** not a good idea ***
    mem_define_io(CASLASTSINE_AD_CAS, CASLASTSINE_AD_CAS, io_handler_cas_tape); // *** not a good idea ***

    memset(cas_stream_buffer, 0, CAS_STREAM_SIZE);
}

/*------------------------------------------------
 * io_handler_cas_tape()
 *
 *  IO call-back handler for CasLastSine variable at address $xx85
 *
 *  This call-back will trigger when reads or writes are directed to
 *  this memory address in the Dragon RAM variable area. Access to this memory
 *  location only happens when bytes are output to cassette tape, and the emulation
 *  will use this as a signal to save output data.
 *
 * This Dragon 32 ROM routine sends the Acc register to tape:
 * 
 * CasByteOut:
 * LBE12   PSHS    A                ; save output byte
 *         LDB     #$01             ; B contains the bitmask that is used to determine
 *                                  ; if the corresponding bit of A is set or not 
 *
 * LBE16   LDY     #TapeSineTab     ; point Y at sine wave table
 *         LDA     <CasLastSine     ; get the value of the last sine
 *         STA     PIA1DA           ; send sine value to D/A via PIA
 *
 *         BITB    ,S               ; test the bit in byte to send
 *         BNE     LBE30            ; if it's a 1 do high frequency
 *
 * ; low frequency lookup
 * LBE23   LDA     ,Y+              ; get next sine value
 *         CMPY    #EndTapeSineTab  ; end of sine table?
 *         BCC     LBE3D            ; yes, skip on
 *         STA     PIA1DA           ; send sine value to D/A via PIA
 *         BRA     LBE23            ; loop for next value
 *
 * ; high frequency lookup
 * LBE30   LDA     ,Y++             ; get next sine value
 *         CMPY    #EndTapeSineTab  ; end of sine table?
 *         BCC     LBE3D            ; yes, skip on
 *         STA     PIA1DA           ; send sine value to D/A via PIA
 *         BRA     LBE30            ; loop for next value
 *
 * LBE3D   STA     <CasLastSine     ; save last sine value sent
 *         ASLB                     ; move on to next bit of byte to send
 *         BCC     LBE16            ; carry will be set when byte is done,
 *                                  ; else loop again for the next bit
 *         PULS    A,PC             ; restore and return
 * 
 *  param:  Call address, data byte for write operation, and operation type
 *  return: Status or data byte
 */
static uint8_t io_handler_cas_tape(uint16_t address, uint8_t data, mem_operation_t op)
{
    uint8_t             data_byte;
    int                 cas_file_stream_complete = 0;

    static cas_tape_state_t tape_state = CAS_STREAM_IDLE;
    static int          count = 0;
    static cpu_state_t  cpu_state;

    static char         file_name[9];
    static int          i = 0;

    /* Count access to 'CasLastSine'.
     * A count of 16 is a single byte writted to tape.
     */
    cpu_get_state(&cpu_state);

    if ( cpu_state.pc == 0xbe1c || cpu_state.pc == 0xbe3f )
    {
        count++;
    }

    if ( count == 16 )
    {
        /* Capture the data byte off of the CPU stack and save to a buffer,
         * then track data stream for file name, data, and end-of-block.
         */
        count = 0;
        data_byte = (uint8_t) mem_read(cpu_state.s);

        /* Store all bytes from the stream into a buffer.
         */
        cas_stream_buffer[i] = data_byte;
        dbg_printf(2, "io_handler_cas_tape()[%d]: io_handler_cas_tape, byte[%d]=0x%02x \n", __LINE__, i, data_byte);

        i++;
        if ( i == CAS_STREAM_SIZE )
        {
            i = CAS_STREAM_SIZE - 1;
        }

        switch ( tape_state )
        {
            case CAS_STREAM_IDLE:
                if ( data_byte == 0x55 )
                {
                    tape_state = CAS_STREAM_LEADER;
                }
                break;

            case CAS_STREAM_LEADER:
                if ( data_byte == 0x55 )
                {
                    tape_state = CAS_STREAM_LEADER;
                }
                else if ( data_byte == 0x3c )
                {
                    tape_state = CAS_STREAM_SYNC;
                }
                else
                {
                    tape_state = CAS_STREAM_IDLE;
                }
                break;

            case CAS_STREAM_SYNC:
                if ( data_byte == 0x00 )
                {
                    tape_state = CAS_STREAM_HEADER;
                }
                else if ( data_byte == 0x01 )
                {
                    tape_state = CAS_STREAM_DATA;
                }
                else if ( data_byte == 0xff )
                {
                    tape_state = CAS_STREAM_EOF;
                }
                else
                {
                    tape_state = CAS_STREAM_IDLE;
                }
                break;

            case CAS_STREAM_HEADER:
                tape_state = cas_stream_header(data_byte, file_name);
                break;

            case CAS_STREAM_DATA:
                tape_state = cas_stream_data(data_byte);
                break;

            case CAS_STREAM_EOF:
                tape_state = cas_stream_eof(data_byte);
                break;

            case CAS_STREAM_WRITE:
                cas_file_stream_complete = 1;
                tape_state = CAS_STREAM_IDLE;
                break;
        }

        /* If an EOF block was encountered then copy the buffer
         * to the SD card as a CAS file.
         */
        if ( cas_file_stream_complete )
        {
            dbg_printf(2, "io_handler_cas_tape()[%d]: io_handler_cas_tape, file='%s'\n", __LINE__, file_name);
            dbg_printf(2, "io_handler_cas_tape()[%d]: io_handler_cas_tape, fat32_fwrite()=%d\n", __LINE__, loader_tape_fwrite(cas_stream_buffer, i));
            i = 0;
            memset(cas_stream_buffer, 0, CAS_STREAM_SIZE);
            //fat32_fclose(&cas_file);
        }

        dbg_printf(2, "io_handler_cas_tape()[%d]: io_handler_cas_tape, state=%d\n", __LINE__, tape_state);
    }

    return data;
}

/*------------------------------------------------
 * cas_stream_header()
 *
 *  Parse the cassette tape stream header to extract the file name.
 *  Note: The function will remove any character that is not a-z, A-Z, or 0-9.
 *
 *  param:  Data byte from stream, file name character buffer
 *  return: tape file write state
 */
static cas_tape_state_t cas_stream_header(uint8_t data_byte, char* file_name)
{
    cas_tape_state_t    result;
    static int  byte_count = -1;
    static int  char_index = 0;

    /* On first entry, the byte will be the header stream length.
     * Get it, and stay in header stream state.
     */
    if ( byte_count < 0 )
    {
        byte_count = data_byte;
        char_index = 0;
        result = CAS_STREAM_HEADER;
    }

    /* Decrement byte stream count past the checksum byte (count will be back to -1)
     * then change state back to receiving a leader byte.
     * Collect file name characters from stream.
     */
    else
    {
        byte_count--;

        result = CAS_STREAM_HEADER;

        if ( byte_count < 0 )
        {
            result = CAS_STREAM_LEADER;
        }
        else if ( char_index >= 0 && char_index <=7 && isalnum(data_byte) )
        {
            file_name[char_index] = data_byte;
            file_name[char_index+1] = 0;
            char_index++;
        }
    }

    return result;
}

/*------------------------------------------------
 * cas_stream_data()
 *
 *  Skip cassette tape data byte stream.
 *
 *  param:  Data byte from stream
 *  return: Tape file write state
 */
static cas_tape_state_t cas_stream_data(uint8_t data_byte)
{
    cas_tape_state_t    result;
    static int  byte_count = -1;

    /* On first entry, the byte will be the data stream length.
     * Get it, and stay in data stream state.
     */
    if ( byte_count < 0 )
    {
        byte_count = data_byte;
        result = CAS_STREAM_DATA;
    }

    /* Decrement byte stream count past the checksum byte (count will be back to -1)
     * then change state back to receiving a leader byte.
     */
    else
    {
        byte_count--;
        if ( byte_count < 0 )
        {
            result = CAS_STREAM_LEADER;
        }
        else
        {
            result = CAS_STREAM_DATA;
        }
    }

    return result;
}

/*------------------------------------------------
 * cas_stream_eof()
 *
 *  Skip cassette tape EOF block byte stream.
 *
 *  param:  Data byte from stream
 *  return: Tape file write state
 */
static cas_tape_state_t cas_stream_eof(uint8_t data_byte)
{
    cas_tape_state_t    result;
    static int  byte_count = -1;

    /* On first entry, the byte will be the EOF length (0 bytes).
     * Get it, and stay in EOF stream state.
     */
    if ( byte_count < 0 )
    {
        byte_count = data_byte;
        result = CAS_STREAM_EOF;
    }

    /* Decrement byte count past the checksum byte (count will be back to -1)
     * then change state to save the file.
     */
    else
    {
        byte_count--;
        if ( byte_count < 0 )
        {
            result = CAS_STREAM_WRITE;
        }
        else
        {
            result = CAS_STREAM_EOF;
        }
    }

    return result;
}