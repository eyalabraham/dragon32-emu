/********************************************************************
 * dbgmsg.h
 *
 *  Debug message output module.
 * 
 *  August 2024
 *
 *******************************************************************/

#ifndef __DBGMSG_H__
#define __DBGMSG_H__

/* Debug level:
   Define as 'DEBUG_LVL'
   0, Errors
   1, Warnings
   2, Information

 */

int dbg_printf(int lvl, const char* format, ...);

#endif  /* __DBGMSG_H__ */