/********************************************************************
 * trace.c
 *
 *  Trace functions' module.
 *
 *  January 30, 2021
 *
 *******************************************************************/

#include    <stdlib.h>
#include    <stdio.h>
#include    <ctype.h>
#include    <string.h>

#include    "mem.h"
#include    "trace.h"

/* -----------------------------------------
   Local definitions
----------------------------------------- */
#define     MAX_TOKENS      3       // Max number of command line tokens
#define     CMD_DELIM       " \t"   // Command line white-space delimiters

/* -----------------------------------------
   Module static functions
----------------------------------------- */
static void print_decorated_cc(uint8_t cc);

/* -----------------------------------------
   Module globals
----------------------------------------- */

/*------------------------------------------------
 * trace_action()
 *
 *  Pause after break-point to accept user commands:
 *
 *  m <start> <end>                  -  Display memory between <start> and <end>
 *  r [a|b|d|x|y|u|s|dp|cc|pc] <val> -  Change register value
 *  g <address>                      -  Run to next breakpoint
 *  <cr>                             -  Step program command on next PC
 * 
 *  param:  Pointer to breakpoint address
 *  return: Breakpoint trigger 0=reset the trigger for another trap, 1=don't reset the trigger.
 */
int trace_action(uint16_t* breakpoint_address)
{
    char        input_string[20];
    char       *tokens[MAX_TOKENS] = {0, 0, 0};
    char       *token;
    char       *temp_cli;
    int         num_tokens;
    int         exit_trace = 0;
    int         i;
    uint8_t     c;
    uint16_t    start, end;

    while ( !exit_trace )
    {
        printf(">");

        fgets(input_string, sizeof(input_string), stdin);
        input_string[strlen(input_string)-1] = 0;

        /* Separate command line into tokens
         */
        temp_cli = input_string;
        for ( num_tokens = 0; num_tokens < MAX_TOKENS; num_tokens++, temp_cli = 0L)
        {
            token = strtok(temp_cli, CMD_DELIM);
            if ( token == 0L )
                break;
            tokens[num_tokens] = token;
        }

        /* If nothing found then this is an empty line, just exit
         */
        if ( num_tokens == 0 )
        {
            exit_trace = 1;
            i = 1;
        }

        /* Parse and execute commands
         */
        else if ( strcmp(tokens[0], "m") == 0L )
        {
            start = ((uint16_t) strtol(tokens[1], 0L, 16)) & 0xfff0;
            end = (((uint16_t) strtol(tokens[2], 0L, 16)) & 0xfff0) + 0x000f;
            if ( end < start )
            {
                printf("End address less than start address.\n");
            }
            else
            {
                for ( i = start; i <= end; i++ )
                {
                    if ( (i % 16) == 0 )
                        printf("\n%04x:", (uint16_t) i);
                    else if ( (i % 8) == 0)
                        printf(" -");

                    c = (uint8_t) mem_read(i);
                    printf(" %02x", c);
                }
                printf("\n");
            }
        }
        else if ( strcmp(tokens[0], "r") == 0L )
        {
            printf("Not implemented.\n");
        }
        else if ( strcmp(tokens[0], "g") == 0L )
        {
            *breakpoint_address = ((uint16_t) strtol(tokens[1], 0L, 16));
            exit_trace = 1;
            i = 0;
        }
        else
        {
            printf("Unknown command.\n");
        }
    }

    return i;
}

/*------------------------------------------------
 * trace_print_registers()
 *
 *  Print CPU state from CPU state structure.
 *
 *  param:  Pointer to CPU state structure
 *  return: None
 */
void trace_print_registers(cpu_state_t* state)
{
    int     i, bytes;

    bytes = state->last_opcode_bytes;

    /* Print opcode mnemonic at PC
     */
    printf("%04X: (%s) ", state->last_pc, cpu_get_menmonic(state->last_pc));

    /* Print opcode and operand bytes
     */
    for ( i = 0; i < bytes; i++ )
    {
        printf("%02x ", mem_read(state->last_pc + i));
    }

    /* Print register content resulting from execution
     */
    printf("\na=0x%02x b=0x%02x x=0x%04x y=0x%04x ", state->a, state->b, state->x, state->y);
    print_decorated_cc(state->cc);
    printf("\n");
    printf("dp=0x%02x u=0x%04x s=0x%04x pc=0x%04x\n", state->dp, state->u, state->s, state->pc);
}

/*------------------------------------------------
 * print_decorated_cc()
 *
 *  Print CPU flags as upper (set) / lower (clear) case characters.
 *
 *  param:  CC flags value packed byte
 *  return: None
 */
static void print_decorated_cc(uint8_t cc)
{
    static  char cc_flag_set[] = {'C', 'V', 'Z', 'N', 'I', 'H', 'F', 'E'};
    static  char cc_flag_clr[] = {'c', 'v', 'z', 'n', 'i', 'h', 'f', 'e'};

    int     i;

    for ( i = 7; i > -1; i--)
    {
        if ( (1 << i) & cc )
            printf("%c", cc_flag_set[i]);
        else
            printf("%c", cc_flag_clr[i]);
    }
}
