/********************************************************************
 * config.h
 *
 *  System-wide configuration.
 *
 *  May 2024
 *
 *******************************************************************/

#ifndef __CONFIG_H__
#define __CONFIG_H__

#define     SD_CARD_BIT_RATE    1000000     // Hz

#if (RPI_BARE_METAL==0)
    #define     DEBUG_LVL       2           // 0=Errors, 1=Warnings, 2=Info
#else
    #define     DEBUG_LVL       1
#endif

#endif  /* __CONFIG_H__ */