/********************************************************************
 * dbgmsg.c
 *
 *  Debug message output module.
 * 
 *  August 2024
 *
 *******************************************************************/

#include    <stdarg.h>

#include    "config.h"
#include    "printf.h"

int dbg_printf(int lvl, const char* format, ...)
{
    int         result = 0;
    va_list     va;

    if ( lvl <= DEBUG_LVL )
    {
        va_start(va, format);
        result = vprintf(format, va);
        va_end(va);
    }

    return result;
}