/********************************************************************
 * fat32.c
 *
 *  FAT32 file system driver.
 *  Minimal implementation, FAT32 file and directory read and write.
 *  Long file names are supported, but *not* long directory names.
 *  The goal is functionality not performance performance implementing only
 *  the functionality needed for future projects.
 *
 *  April 2024
 *
 *******************************************************************/

#include    <string.h>
#include    <stdio.h>
#include    <assert.h>

#include    "sd.h"
#include    "fat32.h"

/* -----------------------------------------
   Module definition
----------------------------------------- */

#define     FAT32_SEC_SIZE          512         // Bytes
#define     FAT32_MAX_SEC_PER_CLUS  16          // *** 1, 2, 4, 8, 16, 32, 64, 128
#define     FAT32_CLUS_PER_SECTOR   (FAT32_SEC_SIZE/sizeof(uint32_t))
#define     FAT32_FAT_MASK          0x0fffffff
#define     FAT32_FREE_CLUSTER      0x00000000
#define     FAT32_FREE_RES1         0x00000001
#define     FAT32_VALID_CLUST_LOW   0x00000002
#define     FAT32_VALID_CLUST_HIGH  0x0fffffef
#define     FAT32_FREE_RES2_LOW     0x0ffffff0
#define     FAT32_FREE_RES2_HIGH    0x0ffffff6
#define     FAT32_BAD_SEC_CLUSTER   0x0ffffff7
#define     FAT32_END_OF_CHAIN      0x0ffffff8

#define     FILE_ATTR_READ_ONLY     0b00000001
#define     FILE_ATTR_HIDDEN        0b00000010
#define     FILE_ATTR_SYSTEM        0b00000100
#define     FILE_ATTR_VOL_LABEL     0b00001000
#define     FILE_ATTR_DIRECTORY     0b00010000
#define     FILE_ATTR_ARCHIVE       0b00100000
#define     FILE_ATTR_LONG_NAME     0b00001111

#define     FILE_LFN_END            0x40

#define     MIN(a, b) (( a < b ) ? a : b)

/* -----------------------------------------
   Module types
----------------------------------------- */
/* Partition table structure
 */
typedef struct
    {
        uint8_t   status;
        uint8_t   first_head;
        uint8_t   first_sector;
        uint8_t   first_cylinder;
        uint8_t   type;
        uint8_t   last_head;
        uint8_t   last_sector;
        uint8_t   last_cylinder;
        uint32_t  first_lba;
        uint32_t  num_sectors;
    } __attribute__ ((packed)) partition_t;

/* boot sector and BPB
 * https://en.wikipedia.org/wiki/Design_of_the_FAT_file_system#BPB
 */
typedef struct
    {
        uint16_t  bytes_per_sector;         // DOS 2.0 BPB
        uint8_t   sectors_per_cluster;
        uint16_t  reserved_sectors;
        uint8_t   fat_count;
        uint16_t  root_directory_entries;
        uint16_t  total_sectors;
        uint8_t   media_descriptor;
        uint16_t  sectors_per_fat;
        uint16_t  sectors_per_track;        // DOS 3.31 BPB
        uint16_t  heads;
        uint32_t  hidden_sectors;
        uint32_t  total_logical_sectors;
        uint32_t  logical_sectors_per_fat;  // FAT32 extensions start here
        uint16_t  drive_desc;
        uint16_t  version;
        uint32_t  cluster_number_root_dir;
        // ... there is more.
    } __attribute__ ((packed)) bpb_t;

typedef struct
{
    char        short_dos_name[8];
    char        short_dos_ext[3];
    uint8_t     attribute;
    uint8_t     user_attribute;
    uint8_t     delete_attribute;
    uint16_t    create_time;
    uint16_t    create_date;
    uint16_t    last_access_date;
    uint16_t    fat32_high_cluster;
    uint16_t    last_mod_time;
    uint16_t    last_mod_date;
    uint16_t    fat32_low_cluster;
    uint32_t    file_size_bytes;
} __attribute__ ((packed)) dir_record_t;

/* -----------------------------------------
   Module functions
----------------------------------------- */
static error_t  fat32_read_sector(uint32_t lba, uint8_t *buffer, uint16_t buffer_len);
static error_t  fat32_write_sector(uint32_t lba, uint8_t *buffer, uint16_t buffer_len);
static error_t  fat32_get_next_cluster_num(uint32_t cluster_num, uint32_t *next_cluster_num);
static error_t  fat32_get_new_cluster(uint32_t *new_cluster_num);
static error_t  fat32_update_cluster_chain(uint32_t cluster_num, uint32_t new_cluster_num);
static error_t  fat32_update_file_size(file_param_t *file, uint32_t file_size);
static uint32_t fat32_get_cluster_base_lba(uint32_t);

static int      dir_get_sfn(dir_record_t *dir_record, char *name, uint16_t name_length);
static int      dir_get_lfn(dir_record_t *dir_record, char *name, uint16_t name_length);

/* -----------------------------------------
   Module globals
----------------------------------------- */
static int      fat32_initialized = 0;

static uint8_t  temp_sector_buffer[(2 * FAT32_SEC_SIZE)];
static uint8_t *low_sector_buffer;
static uint8_t *high_sector_buffer;
static uint32_t absolute_lba_cached = 0;

static struct fat_param_t
{
    uint32_t    first_lba;
    uint32_t    fat_begin_lba;
    uint32_t    cluster_begin_lba;
    uint8_t     sectors_per_cluster;
    uint32_t    sectors_per_fat;
    uint32_t    root_dir_first_cluster;
} fat32_parameters;

/* -------------------------------------------------------------
 * fat32_init()
 *
 *  Initialize FAT32 module
 *
 *  Param:  SD card initialize '=1' or skip '=0'
 *  Return: Driver error
 */
error_t fat32_init(void)
{
    partition_t partitions[4];
    bpb_t       bpb;
    error_t     result;

    if ((result = sd_init()) != NO_ERROR)
    {
        return result;
    }

    /* Initialize two buffer pointers
     */
    low_sector_buffer = &temp_sector_buffer[0];
    high_sector_buffer = &temp_sector_buffer[FAT32_SEC_SIZE];

    /* Read data block
     */
    if ( sd_read_block(0, low_sector_buffer, FAT32_SEC_SIZE) != NO_ERROR )
    {
        return FAT_READ_FAIL;
    }

    /* Analyze boot sector and partition table
     */
    memcpy(partitions, &low_sector_buffer[446], sizeof(partitions));

    if ( low_sector_buffer[510] != 0x55 || low_sector_buffer[511] != 0xaa )
    {
        return FAT_BAD_SECTOR_SIG;
    }

    /* Read file system
     */

    if ( partitions[0].type != 0x0c )
    {
        return FAT_BAD_PARTITION_TYPE;
    }

    fat32_parameters.first_lba = partitions[0].first_lba;

    if ( sd_read_block(fat32_parameters.first_lba, low_sector_buffer, FAT32_SEC_SIZE) != NO_ERROR )
    {
        return FAT_READ_FAIL;
    }

    /* Analyze BPB of first partition
     */
    memcpy(&bpb, &low_sector_buffer[11], sizeof(bpb_t));

    if ( low_sector_buffer[510] != 0x55 || low_sector_buffer[511] != 0xaa )
    {
        return FAT_BAD_SECTOR_SIG;
    }

    if ( bpb.sectors_per_cluster > FAT32_MAX_SEC_PER_CLUS )
    {
        return FAT_BAD_SECTOR_PER_CLUS;
    }

    /* Required FAT32 parsing parameters
     */
    fat32_parameters.fat_begin_lba = fat32_parameters.first_lba + bpb.reserved_sectors;
    fat32_parameters.cluster_begin_lba = fat32_parameters.fat_begin_lba + (bpb.fat_count * bpb.logical_sectors_per_fat);
    fat32_parameters.sectors_per_cluster = bpb.sectors_per_cluster;
    fat32_parameters.root_dir_first_cluster = bpb.cluster_number_root_dir;
    fat32_parameters.sectors_per_fat = bpb.logical_sectors_per_fat;

    fat32_initialized = 1;

    return NO_ERROR;
}


/* -------------------------------------------------------------
 * fat32_close()
 *
 *  Close FAT32 module
 *
 *  NOTE: All open file structures will be invalidated!
 *        Be sure to fat32_fclose() all of them before calling fat32_close()
 *
 *  Param:  None
 *  Return: None
 */
void fat32_close(void)
{
    if ( !fat32_initialized )
        return;

    sd_close();
    fat32_initialized = 0;
}

/* -------------------------------------------------------------
 * fat32_is_initialized()
 *
 *  Check if FAT32 (and SD, SPI) are ready for file IO
 *
 *  Param:  None
 *  Return: "1"=FAT32 initialized, "0"=not initialized
 */
int fat32_is_initialized(void)
{
    return fat32_initialized;
}

/* -------------------------------------------------------------
 * fat32_get_rootdir_cluster()
 *
 *  Get the FAT32 cluster number of the root directory
 *
 *  Param:  None
 *  Return: FAT32 cluster number of the root directory, 0 if error
 */
uint32_t fat32_get_rootdir_cluster(void)
{
    if ( !fat32_initialized )
        return 0;

    return fat32_parameters.root_dir_first_cluster;
}

/* -------------------------------------------------------------
 * fat32_parse_dir()
 *
 *  Parse directory listing from input cluster into directory list array
 *  supplied by the caller.
 *  If count of parsed items is less than 'dir_list_length', then the
 *  function reached the end of the listing.
 *  If 'start_cluster' is '-1' the function will continue to read entries following
 *  the one last read, unless no more entries exist.
 *
 *  Param:  Directory's start cluster number, pointer to directory list array, and its length
 *  Return: Count of parsed items or error code
 */
int fat32_parse_dir(uint32_t start_cluster, dir_entry_t *directory_list, uint16_t dir_list_length)
{
    static uint32_t dir_cluster_num;        // Cluster number
    static uint32_t dir_base_cluster_lba;   // LBA of first sector in the cluster
    static int      dir_sector_num;         // Sector within cluster
    static int      dir_record_num;         // Directory record within sector
    static int      done = 0;

    int             i;                      // Directory list index
    int             long_filename_flag;

    dir_record_t   *dir_record;
    error_t         result;

    if ( !fat32_initialized )
        return FAT_READ_FAIL;

    /* Prevent iteration if the listing was completed before.
     * Reset by invoking with a valid cluster number.
     */
    if ( (start_cluster == -1) && done )
        return 0;

    /* Calculate LBA of the start of the cluster
     * and so an initial read.
     */
    if ( start_cluster != -1 )
    {
        done = 0;

        dir_cluster_num = start_cluster;
        dir_base_cluster_lba = fat32_get_cluster_base_lba(dir_cluster_num);
        dir_sector_num = 0;
        dir_record_num = 0;

        result = fat32_read_sector(dir_base_cluster_lba, high_sector_buffer, FAT32_SEC_SIZE);
        if ( result != NO_ERROR )
        {
            return result;
        }
    }

    long_filename_flag = 0;

    /* Read the cluster one sector at a time and parse
     */
    i = 0;

    while ( i < dir_list_length )
    {
        dir_record = (dir_record_t*) (high_sector_buffer + (dir_record_num * sizeof(dir_record_t)));
        directory_list[i].sfn[0] = 0;
        directory_list[i].lfn[0] = 0;

        /* Done
         */
        if ( dir_record->short_dos_name[0] == 0 )
        {
            done = 1;
            break;
        }

        /* Flag long file name records
         */
        if ( (dir_record->attribute & FILE_ATTR_LONG_NAME) == FILE_ATTR_LONG_NAME )
        {
            long_filename_flag = 1;
        }

        /* Process directory record while skipping volume labels and deleted files
         */
        if ( dir_record->short_dos_name[0] != 0xe5 &&
             dir_record->attribute != FILE_ATTR_VOL_LABEL &&
            (dir_record->attribute & FILE_ATTR_LONG_NAME) != FILE_ATTR_LONG_NAME )
        {
            directory_list[i].is_directory = ((dir_record->attribute & FILE_ATTR_DIRECTORY) ? 1 : 0);
            dir_get_sfn(dir_record, directory_list[i].sfn, FAT32_DOS_FILE_NAME);

            if ( dir_record->short_dos_name[0] == '.' )
            {
                directory_list[i].lfn[0] = '.';
                directory_list[i].lfn[1] = 0;
                if ( dir_record->short_dos_name[1] == '.' )
                {
                    directory_list[i].lfn[1] = '.';
                    directory_list[i].lfn[2] = 0;
                }
            }

            if ( long_filename_flag )
            {
                dir_get_lfn(dir_record, directory_list[i].lfn, FAT32_LONG_FILE_NAME);
                long_filename_flag = 0;
            }
            else
            {
                strncpy(directory_list[i].lfn, directory_list[i].sfn, FAT32_DOS_FILE_NAME);
            }

            directory_list[i].cluster_chain_head = ((uint32_t)dir_record->fat32_high_cluster << 16) + dir_record->fat32_low_cluster;
            /* This adjustment is necessary because the first
             * sub-directory level has a '..' file with a
             * '0' cluster number.
             */
            if ( directory_list[i].cluster_chain_head == 0 )
                directory_list[i].cluster_chain_head = 2;

            directory_list[i].file_size = dir_record->file_size_bytes;

            /* Save references to the directory record location
             * to allow access for modification.
             */
            directory_list[i].dir_record_index = dir_record_num;
            directory_list[i].dir_record_lba = dir_base_cluster_lba + dir_sector_num;

            i++;
        }

        dir_record_num++;

        /* If all records in current sector were parsed
         * and there is still room left in the directory list then try next sector.
         */
        if ( dir_record_num == (FAT32_SEC_SIZE / sizeof(dir_record_t)) )
        {
            dir_record_num = 0;
            dir_sector_num++;

            /* Reached last sector of the cluster then
             * try next cluster until last cluster is reached.
             */
            if ( dir_sector_num == fat32_parameters.sectors_per_cluster )
            {
                dir_sector_num = 0;
                fat32_get_next_cluster_num(dir_cluster_num, &dir_cluster_num);
                /* TODO: check and handle other possible values
                 */
                if ( dir_cluster_num >= FAT32_END_OF_CHAIN )
                    break;
                dir_base_cluster_lba = fat32_get_cluster_base_lba(dir_cluster_num);
            }

            /* Read next sector of directory information.
             * Use a rolling two-buffer schema to allow parsing
             * entries that span sector boundary.
             */
            memcpy(low_sector_buffer, high_sector_buffer, FAT32_SEC_SIZE);
            result = fat32_read_sector((dir_base_cluster_lba + dir_sector_num), high_sector_buffer, FAT32_SEC_SIZE);
            if ( result != NO_ERROR )
            {
                return result;
            }
        }
    } /* while ( i < dir_list_length ) */

    return i;
}

/* -------------------------------------------------------------
 * fat32_fcreate()
 *
 *  TODO: Stub for file or directory creation.
 *  Create a file or directory passed by name, in the parent
 *  directory pointed to by 'directory'.
 *
 *  Param:  File or directory name
 *  Return: Error code
 */
error_t fat32_fcreate(char *file_name, dir_entry_t *directory)
{
    return FAT_FILE_NOT_FOUND;
}

/* -------------------------------------------------------------
 * fat32_fdelete()
 *
 *  TODO: Stub for file or directory deletion.
 *  Delete the file or directory pointed to by 'file_dir_info'.
 *  If the input parameter points to a directory, the directory must be empty.
 *
 *  Param:  Pointed to file or directory entry structure
 *  Return: Error code
 */
error_t fat32_fdelete(dir_entry_t *file_dir_info)
{
    return FAT_FILE_NOT_FOUND;
}

/* -------------------------------------------------------------
 * fat32_fopen()
 *
 *  Open a file for reading. File to open is designated
 *  via a directory entry obtained by calling fat32_parse_dir()
 *  and not its name/location.
 *  When a file is open it can be read from by fat32_fread(),
 *  or written to by fat32_fwrite(), starting at the file position pointer.
 *  The file position pointer can be changed with fat32_fseek().
 *
 *  Example: call fat32_parse_dir(), find the
 *  file you want to access, and pass that entry to fat32_fopen().
 *
 *  Param:  Pointed to file's directory entry structure, pointer to empty file structure.
 *  Return: Error code
 */
error_t fat32_fopen(dir_entry_t *directory_entry, file_param_t *file_parameters)
{
    if ( directory_entry->is_directory || file_parameters->file_is_open )
        return FAT_FILE_OPEN_ERR;

    file_parameters->file_is_open = 1;
    file_parameters->file_start_cluster = directory_entry->cluster_chain_head;
    file_parameters->file_size = directory_entry->file_size;
    file_parameters->current_position = 0;
    file_parameters->current_cluster = file_parameters->file_start_cluster;
    file_parameters->is_end_of_chain = 0;   // Can't know at this point, need to use seek.
    file_parameters->current_base_lba = fat32_get_cluster_base_lba(file_parameters->file_start_cluster);;
    file_parameters->current_lba_index = 0;
    file_parameters->current_byte_index = 0;
    file_parameters->eof_flag = 0;
    file_parameters->sector_cached = 0;
    file_parameters->dir_record_index = directory_entry->dir_record_index;
    file_parameters->dir_record_lba = directory_entry->dir_record_lba;

    if ( file_parameters->file_size == 0 )
    {
        file_parameters->eof_flag = 1;
    }
    else
    {
        file_parameters->eof_flag = 0;
    }

    return NO_ERROR;
}

/* -------------------------------------------------------------
 * fat32_fclose()
 *
 *  Close a file by resetting its parameter structure.
 *
 *  Param:  Pointer to an open file structure.
 *  Return: None
 */
void fat32_fclose(file_param_t *file_parameters)
{
    file_parameters->file_is_open = 0;
    file_parameters->file_start_cluster = 0;
    file_parameters->file_size = 0;
    file_parameters->current_position = 0;
    file_parameters->current_cluster = 0;
    file_parameters->is_end_of_chain = 0;
    file_parameters->current_base_lba = 0;
    file_parameters->current_lba_index = 0;
    file_parameters->current_byte_index = 0;
    file_parameters->eof_flag = 0;
    file_parameters->sector_cached = 0;
    file_parameters->dir_record_index = 0;
    file_parameters->dir_record_lba = 0;
}

/* -------------------------------------------------------------
 * fat32_fseek()
 *
 *  Set file read position for the next read command.
 *  The function reads the sector containing the next byte to read.
 *
 *  Param:  Pointer to an open file structure, 0-based index file byte position.
 *  Return: Error code
 */
error_t fat32_fseek(file_param_t *file_parameters, uint32_t byte_position)
{
    int         i;
    int         cluster_index;
    uint32_t    current_cluster_num;

    if ( !file_parameters->file_is_open )
        return FAT_FILE_NOT_OPEN;

    /* Handle possible out of range condition if
     * seeking past end-of-file.
     */
    if ( byte_position > file_parameters->file_size )
    {
        return FAT_FILE_SEEK_RANGE;
    }

    /* Traves cluster linked list in FAT to find the cluster and the of the
     * as LBA's index within that cluster of the requested 'byte_position'.
     */
    cluster_index = byte_position / (fat32_parameters.sectors_per_cluster * FAT32_SEC_SIZE);

    current_cluster_num = file_parameters->file_start_cluster;

    file_parameters->current_cluster = current_cluster_num;
    file_parameters->is_end_of_chain = 0;
    
    for ( i = 0; i < cluster_index; i++ )
    {
        fat32_get_next_cluster_num(current_cluster_num, &current_cluster_num);

        /* We hit end-of-chain so we stop here.
         */
        if ( current_cluster_num >= FAT32_END_OF_CHAIN )
        {
            file_parameters->is_end_of_chain = 1;
            break;
        }
        /* Keep walking cluster number chain.
         */
        else if ( current_cluster_num >= FAT32_VALID_CLUST_LOW &&
                  current_cluster_num <= FAT32_VALID_CLUST_HIGH )
        {
            file_parameters->current_cluster = current_cluster_num;
            continue;
        }
        /* Found something other than a valid cluster number or an end-of-chain.
         */
        else
        {
            return FAT_FILE_SEEK_ERR;
        }
    }

    if ( file_parameters->is_end_of_chain )
    {
        file_parameters->current_base_lba = 0;
        file_parameters->current_lba_index = 0;
    }
    else
    {
        file_parameters->current_base_lba = fat32_get_cluster_base_lba(current_cluster_num);
        file_parameters->current_lba_index = (byte_position / FAT32_SEC_SIZE) % fat32_parameters.sectors_per_cluster;
    }
    
    file_parameters->current_byte_index = byte_position % FAT32_SEC_SIZE;
    file_parameters->sector_cached = 0;
    file_parameters->current_position = byte_position;

    if ( byte_position == file_parameters->file_size )
    {
        file_parameters->eof_flag = 1;
    }
    else
    {
        file_parameters->eof_flag = 0;
    }

    return NO_ERROR;
}

/* -------------------------------------------------------------
 * fat32_fread()
 *
 *  Read file data from current position towards end-of-file.
 *  Read stops at end-of-file or when buffer is full.
 *  If buffer is full, then another read will continue from
 *  the byte after the last position that was read.
 *  A fat32_fopen(), and an optional fat32_fseek(), must be called before fat32_fread().
 *
 *  Param:  Pointer to an open file structure, buffer for file data, and the buffer length
 *  Return: Byte count read, 0=no more data (reached EOF), or error code
 */
int fat32_fread(file_param_t *file_parameters, uint8_t *buffer, uint16_t buffer_length)
{
    uint16_t    i, j;
    uint32_t    k, r;
    uint16_t    byte_count;
    uint32_t    file_position;  // Byte position in file
    uint32_t    file_cluster;   // Cluster number
    uint32_t    base_lba;       // Cluster base LBA
    uint8_t     lba_index;      // LBA index within cluster
    uint16_t    byte_offset;    // Byte index within current sector
    error_t     result;

    if ( !file_parameters->file_is_open )
        return FAT_FILE_NOT_OPEN;

    if ( file_parameters->eof_flag )
        return FAT_EOF;

    file_position = file_parameters->current_position;
    file_cluster = file_parameters->current_cluster;
    base_lba = file_parameters->current_base_lba;
    lba_index = file_parameters->current_lba_index;
    byte_offset = file_parameters->current_byte_index;

    /* This will address a case where multiple files are open
     * and are accessed alternatly. It will detect the case and force
     * sector cache refresh if the new request assumes a sector that is
     * not already cached.
     */
    if ( absolute_lba_cached != (base_lba + lba_index) )
    {
        file_parameters->sector_cached = 0;
    }

    /* An initial read to cache the first sector of the read sequence.
     * A call to fat32_fopen() or fat32_fseek() invalidates the 'cached' flag
     * so this initial read is needed.
     */
    if ( !file_parameters->sector_cached )
    {
        result = fat32_read_sector((base_lba + lba_index), high_sector_buffer, FAT32_SEC_SIZE);
        if ( result != NO_ERROR )
        {
            return result;
        }
        absolute_lba_cached = base_lba + lba_index;
        file_parameters->sector_cached = 1;
    }

    /* Read sectors and move data into the client read buffer until the buffer
     * if full or we reached end of file. Update file position for next call.
     *
     */
    byte_count = 0;

    while ( byte_count < buffer_length )
    {
        /* Transfer data to client buffer
         */
        i = buffer_length - byte_count;                 // Byte space available in client buffer
        j = FAT32_SEC_SIZE - byte_offset;               // Bytes left to read in cached buffer
        k = file_parameters->file_size - file_position; // Bytes left to read in file

        r = MIN(i, j);
        r = MIN(r, k);

        memcpy(&buffer[byte_count], &high_sector_buffer[byte_offset], r);
        byte_offset += r;
        byte_count += r;
        file_position += r;

        /* Read completed conditions
         */
        if ( file_position == file_parameters->file_size )
        {
            file_parameters->eof_flag = 1;
            break;
        }

        /* Prepare for getting the next sector if we have more bytes to read
         * and we finished reading the current sector.
         */

        /* End of sector
         */
        if ( byte_offset == FAT32_SEC_SIZE )
        {
            byte_offset = 0;
            lba_index++;

            /* End of cluster
             */
            if ( lba_index == fat32_parameters.sectors_per_cluster )
            {
                lba_index = 0;
                file_parameters->is_end_of_chain = 0;
                fat32_get_next_cluster_num(file_cluster, &file_cluster);
                /* Just a guard, should not happen.
                 */
                /* TODO: check and handle other possible values
                 */
                if ( file_cluster >= FAT32_END_OF_CHAIN )
                {
                    file_parameters->is_end_of_chain = 1;
                    file_parameters->eof_flag = 1;
                    break;
                }
                base_lba = fat32_get_cluster_base_lba(file_cluster);
            }
        }

        /* Read next sector into cache
         */
        result = fat32_read_sector((base_lba + lba_index), high_sector_buffer, FAT32_SEC_SIZE);
        if ( result != NO_ERROR )
        {
            return result;
        }
        absolute_lba_cached = base_lba + lba_index;
        file_parameters->sector_cached = 1;
    }

    /* Update file descriptor structure
     */
    file_parameters->current_position = file_position;
    file_parameters->current_cluster = file_cluster;
    file_parameters->current_base_lba = base_lba;
    file_parameters->current_lba_index = lba_index;
    file_parameters->current_byte_index = byte_offset;

    return byte_count;
}

/* -------------------------------------------------------------
 * fat32_fwrite()
 *
 *  Write data to an open file starting at current position towards end-of-file.
 *  Write stops if FAT is full or all bytes in buffer have been written.
 *  Function will allocate clusters as required and update FAT.
 *  A fat32_fcreate() [not implemented] or fat32_fopen(), and an optional fat32_fseek(), must be called before fat32_fwrite().
 *
 *  Note: Internal write errors abort write operation without attempting cleanup.
 *
 *  Param:  Pointer to an open file structure, buffer with data to write, and the buffer length
 *  Return: Byte count written, 0=no more data, or error code
 */
int fat32_fwrite(file_param_t *file_parameters, uint8_t *buffer, uint16_t buffer_length)
{
    uint16_t    i, j, r;
    uint16_t    byte_count;
    uint32_t    file_position;      // Byte position in file
    uint32_t    file_cluster;       // Cluster number
    uint32_t    temp_file_cluster;  // Cluster number
    uint32_t    base_lba;           // Cluster base LBA
    uint32_t    new_file_size;
    uint8_t     lba_index;          // LBA index within cluster
    uint16_t    byte_offset;        // Byte index within current sector
    error_t     result;

    if ( !file_parameters->file_is_open )
        return FAT_FILE_NOT_OPEN;

    /* File with zero-bytes size requires special handling
     * and currently not supported.
     */
    if ( file_parameters->file_size == 0 )
    {
        return FAT_WRITE_FAIL;
    }

    file_position = file_parameters->current_position;
    new_file_size = file_parameters->file_size;
    file_cluster = file_parameters->current_cluster;
    base_lba = file_parameters->current_base_lba;
    lba_index = file_parameters->current_lba_index;
    byte_offset = file_parameters->current_byte_index;

    /* Allocate a new cluster is the 'currnet_cluster is the last one in the chain.
     * Leave 'is_end_of_chain' true becasue the new cluster is still the last one in the chain.
     */
    if ( file_parameters->is_end_of_chain )
    {
        temp_file_cluster = file_cluster;

        result = fat32_get_new_cluster(&file_cluster);
        if ( result != NO_ERROR )
        {
            return result;
        }

        result = fat32_update_cluster_chain(temp_file_cluster, file_cluster);
        if ( result != NO_ERROR )
        {
            return result;
        }

        base_lba = fat32_get_cluster_base_lba(file_cluster);
    }

    /* This will address a case where multiple files are open
     * and are accessed alternatly. It will detect the case and force
     * sector cache refresh if the new request assumes a sector that is
     * not already cached.
     */
    if ( absolute_lba_cached != (base_lba + lba_index) )
    {
        file_parameters->sector_cached = 0;
    }

    /* An initial read to cache the first sector of the write sequence.
     * A call to fat32_fopen() or fat32_fseek() invalidates the 'cached' flag
     * so this initial read is needed.
     */
    if ( !file_parameters->sector_cached )
    {
        result = fat32_read_sector((base_lba + lba_index), high_sector_buffer, FAT32_SEC_SIZE);
        if ( result != NO_ERROR )
        {
            return result;
        }
        
        absolute_lba_cached = base_lba + lba_index;
        file_parameters->sector_cached = 1;
    }

    /* A read-modify-write sequecence of sectors with data from client buffer until the buffer
     * if empty or we reached end of file. Update file position for next call.
     *
     */
    byte_count = 0;

    while ( byte_count < buffer_length )
    {
        /* Transfer data to client buffer, persist to media
         * and adjust indexes.
         */
        i = buffer_length - byte_count;                 // Byte remaining in client buffer
        j = FAT32_SEC_SIZE - byte_offset;               // Bytes to end of cached buffer

        r = MIN(i, j);

        memcpy(&high_sector_buffer[byte_offset], &buffer[byte_count], r);

        result = fat32_write_sector((base_lba + lba_index), high_sector_buffer, FAT32_SEC_SIZE);
        if ( result != NO_ERROR )
        {
            return result;
        }

        byte_offset += r;
        byte_count += r;
        file_position += r;
        if ( file_position > new_file_size )
            new_file_size = file_position;

        /* Write completed conditions
         */
        if ( byte_count == buffer_length )
        {
            break;
        }

        /* Prepare for getting the next sector or allocating a cluster(s)
         * if we have more bytes to write.
         */

        /* End of sector
         */
        if ( byte_offset == FAT32_SEC_SIZE )
        {
            byte_offset = 0;
            lba_index++;

            /* End of cluster then get the next in the chain or
             * allocate a new cluster.
             */
            if ( lba_index == fat32_parameters.sectors_per_cluster )
            {
                lba_index = 0;
                temp_file_cluster = file_cluster;
                file_parameters->is_end_of_chain = 0;

                result = fat32_get_next_cluster_num(file_cluster, &file_cluster);
                if ( result != NO_ERROR )
                {
                    return result;
                }

                if ( file_cluster >=  FAT32_END_OF_CHAIN )
                {
                    file_parameters->is_end_of_chain = 1;

                    result = fat32_get_new_cluster(&file_cluster);
                    if ( result != NO_ERROR )
                    {
                        return result;
                    }

                    result = fat32_update_cluster_chain(temp_file_cluster, file_cluster);
                    if ( result != NO_ERROR )
                    {
                        return result;
                    }
                }

                base_lba = fat32_get_cluster_base_lba(file_cluster);
            }
        }

        /* Read next sector into cache
         */
        result = fat32_read_sector((base_lba + lba_index), high_sector_buffer, FAT32_SEC_SIZE);
        if ( result != NO_ERROR )
        {
            return result;
        }
        absolute_lba_cached = base_lba + lba_index;
        file_parameters->sector_cached = 1;
    }

    /* Update directory with file size
     */
    if ( new_file_size > file_parameters->file_size )
    {
        result = fat32_update_file_size(file_parameters, new_file_size);
        if ( result != NO_ERROR )
        {
            return result;
        }       
    }

    /* Update file descriptor structure
     */
    file_parameters->file_size = new_file_size;
    file_parameters->current_position = file_position;
    file_parameters->current_cluster = file_cluster;
    file_parameters->current_base_lba = base_lba;
    file_parameters->current_lba_index = lba_index;
    file_parameters->current_byte_index = byte_offset;

    if ( file_position == file_parameters->file_size )
    {
        file_parameters->eof_flag = 1;
    }
    else
    {
        file_parameters->eof_flag = 0;
    }

    return byte_count;
}

/* -------------------------------------------------------------
 * fat32_read_sector()
 *
 *  Read a sector (of 'block size') from SD to buffer.
 *  Buffer must be at least one sector worth of bytes in size.
 *
 *  Param:  Sector's LBA, buffer pointer, and its length
 *  Return: Driver error
 */
static error_t fat32_read_sector(uint32_t lba, uint8_t *buffer, uint16_t buffer_len)
{
    if ( !fat32_initialized || buffer_len < FAT32_SEC_SIZE )
        return FAT_READ_FAIL;

    return sd_read_block(lba, buffer, FAT32_SEC_SIZE);
}

/* -------------------------------------------------------------
 * fat32_write_sector()
 *
 *  Write a sector (of 'block size') to SD from buffer.
 *  Buffer must be at least one sector worth of bytes in size.
 *
 *  Param:  Sector's LBA, buffer pointer, and its length
 *  Return: Driver error
 */
static error_t fat32_write_sector(uint32_t lba, uint8_t *buffer, uint16_t buffer_len)
{
    if ( !fat32_initialized || buffer_len < FAT32_SEC_SIZE )
        return FAT_WRITE_FAIL;

    return sd_write_block(lba, buffer, FAT32_SEC_SIZE);
}

/* -------------------------------------------------------------
 * fat32_get_next_cluster_num()
 *
 *  Given a cluster number, scan the FAT32 table and find
 *  the next cluster number in the chain.
 *
 *  Param:  Cluster number to start scan, pointer to next cluster number return variable
 *  Return: error code, if error (i.e. not NO_ERROR) then next cluster retuens FAT32_END_OF_CHAIN
 */
static error_t fat32_get_next_cluster_num(uint32_t cluster_num, uint32_t *next_cluster_num)
{
    error_t     result;
    uint32_t    fat32_sector_lba;
    uint32_t    fat32_sector_offset;

    fat32_sector_lba = fat32_parameters.fat_begin_lba + cluster_num / FAT32_CLUS_PER_SECTOR;

    result = fat32_read_sector(fat32_sector_lba, low_sector_buffer, FAT32_SEC_SIZE);
    if ( result != NO_ERROR )
    {
        *next_cluster_num = FAT32_END_OF_CHAIN;
        return result;
    }

    fat32_sector_offset = (cluster_num % FAT32_CLUS_PER_SECTOR) * sizeof(uint32_t);

    *next_cluster_num = *((uint32_t*)&low_sector_buffer[fat32_sector_offset]) & FAT32_FAT_MASK;
    
    return NO_ERROR;
}

/* -------------------------------------------------------------
 * fat32_get_new_cluster()
 *
 *  Allocate a new cluster for file storage.
 *
 *  Param:  Pointer to new cluster number return variable
 *  Return: Error code.
 *          If error is FAT_CRITICAL_ERR, then fix corrption by:
 *          1. With 'new_cluster_num' scan FAT to find the corrupted entry and change it back to
 *             FAT32_END_OF_CHAIN.
 *          2. Use <lba> = <fat_begin_lba> + <cluster_num> / FAT32_CLUS_PER_SECTOR and
 *             <cluster_entry_index> = (<cluster_num> % FAT32_CLUS_PER_SECTOR) * sizeof(uint32_t)
 *             with 'new_cluster_num' to find the corrupted entry and change it back to
 *             free FAT32_FREE_CLUSTER
 */
static error_t fat32_get_new_cluster(uint32_t *new_cluster_num)
{
    error_t     result;
    int         found_free;
    uint32_t    fat_sector_index;
    uint32_t    cluster_entry_index;
    uint32_t    fat32_sector_lba;
    uint32_t   *cluster_num_list;

    found_free = 0;
    cluster_num_list = (uint32_t*) low_sector_buffer;

    /* Scan sectors in FAT to find a free cluster.
     */
    for ( fat_sector_index = 0; !found_free && fat_sector_index < fat32_parameters.sectors_per_fat; fat_sector_index++ )
    {
        fat32_sector_lba = fat32_parameters.fat_begin_lba + fat_sector_index;

        result = fat32_read_sector(fat32_sector_lba, low_sector_buffer, FAT32_SEC_SIZE);
        if ( result != NO_ERROR )
        {
            *new_cluster_num = 0;
            return result;
        }

        for ( cluster_entry_index = 0; cluster_entry_index < FAT32_CLUS_PER_SECTOR; cluster_entry_index++ )
        {
            if ( (cluster_num_list[cluster_entry_index] & FAT32_FAT_MASK) == FAT32_FREE_CLUSTER )
            {
                found_free = 1;
                break;
            }
        }
    }

    /* Return error is not found, assume no more space on media
     */
    if ( !found_free )
    {
        *new_cluster_num = 0;
        return FAT_OUT_OF_SPACE;
    }

    *new_cluster_num = (fat_sector_index - 1) * FAT32_CLUS_PER_SECTOR + cluster_entry_index;

    return NO_ERROR;
}

/* -------------------------------------------------------------
 * fat32_update_cluster_chain()
 *
 *  Update FAT32 tables chaining 'cluster_num' with 'new_cluster_num'
 *  entries.
 *
 *  Param:  Current cluster number to link, new/next cluster number.
 *  Return: Error code.
 *          If error is FAT_CRITICAL_ERR, then fix corrption by:
 *          1. With 'new_cluster_num' scan FAT to find the corrupted entry and change it back to
 *             FAT32_END_OF_CHAIN.
 *          2. Use <lba> = <fat_begin_lba> + <cluster_num> / FAT32_CLUS_PER_SECTOR and
 *             <cluster_entry_index> = (<cluster_num> % FAT32_CLUS_PER_SECTOR) * sizeof(uint32_t)
 *             with 'new_cluster_num' to find the corrupted entry and change it back to
 *             free FAT32_FREE_CLUSTER
 */
static error_t fat32_update_cluster_chain(uint32_t cluster_num, uint32_t new_cluster_num)
{
    error_t     result;
    uint32_t    fat32_sector_lba;
    uint32_t    cluster_entry_index;
    uint32_t   *cluster_num_list;

    cluster_num_list = (uint32_t*) low_sector_buffer;

    /* Newly allocated cluster entry is the new end-of-chain.
     * Update the entry and store the secor back into FAT.
     */
    fat32_sector_lba = fat32_parameters.fat_begin_lba + new_cluster_num / FAT32_CLUS_PER_SECTOR;
    cluster_entry_index = new_cluster_num % FAT32_CLUS_PER_SECTOR;

    result = fat32_read_sector(fat32_sector_lba, low_sector_buffer, FAT32_SEC_SIZE);
    if ( result != NO_ERROR )
    {
        return result;
    }

    if ( (cluster_num_list[cluster_entry_index] & FAT32_FAT_MASK) != FAT32_FREE_CLUSTER )
    {
        return FAT_WRITE_FAIL;
    }

    cluster_num_list[cluster_entry_index] |= FAT32_END_OF_CHAIN;

    result = fat32_write_sector(fat32_sector_lba, low_sector_buffer, FAT32_SEC_SIZE);
    if ( result != NO_ERROR )
    {
        return result;
    }

    /* Read the sector holding the previous cluster number location.
     * Update it to point to the new cluster number,
     * and store the sector back into FAT.
     * Note: failure to update the sector here means that we have
     *       a potential FAT corruption!
     */
    fat32_sector_lba = fat32_parameters.fat_begin_lba + cluster_num / FAT32_CLUS_PER_SECTOR;

    result = fat32_read_sector(fat32_sector_lba, low_sector_buffer, FAT32_SEC_SIZE);
    if ( result != NO_ERROR )
    {
        return FAT_CRITICAL_ERR;    /*** FAT corruption ***/
    }

    cluster_entry_index = cluster_num % FAT32_CLUS_PER_SECTOR;
    cluster_num_list[cluster_entry_index] &= ~FAT32_FAT_MASK;
    cluster_num_list[cluster_entry_index] |= new_cluster_num;

    result = fat32_write_sector(fat32_sector_lba, low_sector_buffer, FAT32_SEC_SIZE);
    if ( result != NO_ERROR )
    {
        return FAT_CRITICAL_ERR;    /*** FAT corruption ***/
    }

    return NO_ERROR;
}

/* -------------------------------------------------------------
 * fat32_update_file_size()
 *
 *  Update directory record with new file size.
 *
 *  Param:  Open file parameters, new file size
 *  Return: Error code.
 */
static error_t fat32_update_file_size(file_param_t *file, uint32_t file_size)
{
    error_t         result;
    dir_record_t   *directory_record;

    result = fat32_read_sector(file->dir_record_lba, low_sector_buffer, FAT32_SEC_SIZE);
    if ( result != NO_ERROR )
    {
        return FAT_READ_FAIL;
    }
    
    directory_record = (dir_record_t*) low_sector_buffer;
    directory_record[file->dir_record_index].file_size_bytes = file_size;

    result = fat32_write_sector(file->dir_record_lba, low_sector_buffer, FAT32_SEC_SIZE);
    if ( result != NO_ERROR )
    {
        return FAT_CRITICAL_ERR;    /*** FAT corruption ***/
    }

    return NO_ERROR;
}

/* -------------------------------------------------------------
 * fat32_get_cluster_base_lba()
 *
 *  Return the base LBA number (LBA of first sector) of the given cluster.
 *
 *  Param:  Cluster number
 *  Return: LBA of first sector in the cluster
 */
static uint32_t fat32_get_cluster_base_lba(uint32_t cluster)
{
    return (fat32_parameters.cluster_begin_lba + (cluster - 2) * fat32_parameters.sectors_per_cluster);
}

/* -------------------------------------------------------------
 * dir_get_sfn()
 *
 *  Extract short (DOS 8.3) file name from directory record
 *
 *  Param:  Directory record, buffer for file name and its length
 *  Return: Characters extracted
 */
static int dir_get_sfn(dir_record_t *dir_record, char *name, uint16_t name_length)
{
    char   *record;
    int     i, c;

    if ( name_length < FAT32_DOS_FILE_NAME )
        return 0;

    record = (char *) dir_record;

    for ( i = 0, c = 0; i < (FAT32_DOS_FILE_NAME-2); i++ )
    {
        if ( record[i] == 0x20 )
            continue;

        if ( i == 8)
        {
            name[i] = '.';
        }

        name[i] = record[i];
        c++;
    }

    name[c] = 0;

    return c;
}

/* -------------------------------------------------------------
 * dir_get_lfn()
 *
 *  Extract long file name from directory record
 *
 *  Param:  Directory record, buffer for long file name and its length
 *  Return: Characters extracted
 */
static int dir_get_lfn(dir_record_t *dir_record, char *name, uint16_t name_length)
{
    static int  char_locations[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

    char       *record;
    int         i, j;

    if ( name_length < (FAT32_LONG_FILE_NAME-1) )
        return 0;

    j = 0;

    do
    {
        dir_record--;
        record = (char *) dir_record;

        for ( i = 0; i < 13; i++, j++ )
        {
            if ( record[char_locations[i]] == 0xff )
                break;

            name[j] = record[char_locations[i]];
        }
    }
    while ( (dir_record->short_dos_name[0] & 0x40) == 0 );

    name[j] = 0;

    return j;
}
