/********************************************************************
 * fat32.h
 *
 *  FAT32 file system driver header.
 *  Minimal implementation, FAT32 file and directory read and write.
 *  Long file names are supported, but *not* long directory names.
 *  The goal is functionality not performance implementing only
 *  the functionality needed for future projects.
 *
 *  April 2024
 *
 *******************************************************************/

#ifndef __FAT32_H__
#define __FAT32_H__

#include    <stdint.h>

#include    "errors.h"

#define     FAT32_LONG_FILE_NAME    256
#define     FAT32_DOS_FILE_NAME     13
#define     FAT32_ROOT_DIR_CLUSTER  2

typedef struct
{
    int         is_directory;
    char        lfn[FAT32_LONG_FILE_NAME];
    char        sfn[FAT32_DOS_FILE_NAME];
    uint32_t    cluster_chain_head;
    uint32_t    file_size;
    uint8_t     dir_record_index;   // Location of directory record index and LBA,
    uint32_t    dir_record_lba;     // in case we need to access it for updates.
} dir_entry_t;

typedef struct
{
    int         file_is_open;       // File open flag
    uint32_t    file_start_cluster; // Cluster number of the first cluster of the file
    uint32_t    file_size;          // File size in bytes
    uint32_t    current_position;   // Current byte position of read pointer within the file
    uint32_t    current_cluster;    // Current cluster
    int         is_end_of_chain;    // When 'true' then 'current_cluster' is the last cluster in the chain
    uint32_t    current_base_lba;   // Cluster's base LBA
    uint8_t     current_lba_index;  // LBA index within current cluster
    uint16_t    current_byte_index; // Byte index within current LBA
    int         eof_flag;
    int         sector_cached;      // Has a new sector been read and cached in the buffer?
    uint8_t     dir_record_index;   // Location of directory record index and LBA,
    uint32_t    dir_record_lba;     // in case we need to access it for updates.
} file_param_t;

/********************************************************************
 *  FAT32 directory and file access API
 */
error_t     fat32_init(void);
void        fat32_close(void);
int         fat32_is_initialized(void);

uint32_t    fat32_get_rootdir_cluster(void);
int         fat32_parse_dir(uint32_t, dir_entry_t*, uint16_t);

error_t     fat32_fcreate(char*, dir_entry_t*);
error_t     fat32_fdelete(dir_entry_t*);

error_t     fat32_fopen(dir_entry_t*, file_param_t*);
void        fat32_fclose(file_param_t*);
error_t     fat32_fseek(file_param_t*, uint32_t);
int         fat32_fread(file_param_t*, uint8_t*, uint16_t);
int         fat32_fwrite(file_param_t*, uint8_t*, uint16_t);

#endif  /* __FAT32_H__ */
