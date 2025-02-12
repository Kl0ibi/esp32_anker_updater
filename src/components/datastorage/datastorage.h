#ifndef DATASTORAGE_H
#define DATASTORAGE_H

#include <stdint.h>


#define DATASTORAGE_OK 0
#define DATASTORAGE_WAR_NOTFOUND 1
#define DATASTORAGE_ERR_GENERAL 2
#define DATASTORAGE_ERR_NOMEMORYLEFT 3
#define DATASTORAGE_ERR_NOTINIT 4
#define DATASTORAGE_ERR_PARTITION 5
#define DATASTORAGE_ERR_FILE_ERASE 6
#define DATASTORAGE_ERR_FILE_OPEN 7
#define DATASTORAGE_ERR_FILE_WRITE 8
#define DATASTORAGE_ERR_FILE_READ 9

#define DATASTORAGE_WRITE_DATA 0
#define DATASTORAGE_WRITE_BINARY_DATA  1
#define DATASTORAGE_APPEND_DATA 2
#define DATASTORAGE_APPEND_BINARY_DATA 3

#define DATASTORAGE_READ_DATA 0
#define DATASTORAGE_READ_BINARY_DATA 1


uint8_t datastorage_print_all_files_in_partition();


uint8_t datastorage_cross_copy_files_over_storage_partitions();


uint8_t datastorage_migrate_storage_from_spiffs_to_littlefs();


uint8_t datastorage_check_file_existence(const char *fileName, bool *fileStatus);


uint8_t datastorage_erase_file_from_partition(const char *fileName);


uint8_t datastorage_write_data_to_file_in_partition(const char *fileName, uint8_t type, uint8_t *data, uint32_t *cnt);


uint8_t datastorage_read_data_from_file_in_partition(const char *fileName, uint8_t type, uint8_t **data, uint32_t *cnt);


uint8_t datastorage_write_chunked_binary_data_to_file_in_partition(const char *fileName, uint32_t offset, uint8_t *data, uint32_t *cnt, bool lastChunk);


uint8_t datastorage_read_chunked_binary_data_from_file_in_partition(const char *fileName, uint32_t offset, uint8_t *data, uint32_t *cnt);


uint8_t datastorage_clear_chunked_file_cache();


uint8_t datastorage_erase_storage_partition();


uint8_t datastorage_mount_storage_partition(bool systemMount);


uint8_t datastorage_unmount_storage_partition();


#endif
