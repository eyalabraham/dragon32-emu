/********************************************************************
 * loader.h
 *
 *  Header for ROM and CAS file loader
 *
 *  May 11, 2021
 *
 *******************************************************************/

#ifndef __LOADER_H__
#define __LOADER_H__

/* -----------------------------------------
   Types
----------------------------------------- */
typedef enum
    {
        FILE_NONE = 0,
        FILE_ROM,
        FILE_CAS,
        FILE_VDK,
        FILE_DSK,
        FILE_OTHER,
    } loader_file_type_t;

/* -----------------------------------------
   Interface functions
----------------------------------------- */
void loader_init(void);
void loader(void);

int  loader_cas_fread(uint8_t*, uint16_t);

int  loader_disk_fread(uint8_t*, uint16_t);
int  loader_disk_fwrite(uint8_t*, uint16_t);
int  loader_disk_fseek(uint32_t);
loader_file_type_t  loader_disk_img_type(void);

#endif  /* __LOADER_H__ */
