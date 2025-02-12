#include <esp_littlefs.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include "system.h"
#include "datastorage.h"
#include "logging.h"


static const char *TAG = "datastorage";


typedef struct {
	char *name;
	uint8_t *data;
	uint32_t size;
} ram_file_store_t;


char *globFilePath = NULL;
FILE *globFile = NULL;


uint8_t datastorage_print_all_files_in_partition() {
	DIR *dir;
	char *filePath;
	struct dirent *fileDir;
	struct stat fileStat;
	int32_t ret;

	LOGD(TAG, "Listing all files in storage partition");

	dir = opendir("/mcufirm");
	if (dir == NULL) {
		LOGE(TAG, "Failed to open directory in storage partition");
		return DATASTORAGE_ERR_PARTITION;
	}

	while (true) {
		fileDir = readdir(dir);
		if (fileDir == NULL) {
			break;
		}

		ret = asprintf(&filePath, "/%s", fileDir->d_name);
		if (ret < 1) {
			LOGE(TAG, "Failed to allocate memory for file path");
			closedir(dir);

			return DATASTORAGE_ERR_NOMEMORYLEFT;
		}

		ret = stat(filePath, &fileStat);
		if (ret == 0) {
			LOGD(TAG, "%s, size: %dB", filePath, (int)fileStat.st_size);
		}
		else {
			LOGE(TAG, "Failed to get infos for file");
		}
		FREE_MEM(filePath);
	}
	closedir(dir);
	LOGD(TAG, "Finished file listing");

	return DATASTORAGE_OK;
}


uint8_t datastorage_check_file_existence(const char *file_name, bool *file_status) {
    char *temp_file_path = NULL;
	char *file_path;
	FILE *file;
	struct stat file_stat;

	if (file_name == NULL) {
		LOGE(TAG, "Specified file name not initialized");
		return DATASTORAGE_ERR_GENERAL;
	}
	if (asprintf(&file_path, "/mcufirm/%s", file_name) < 1) {
		LOGE(TAG, "Failed to allocate memory for file path");
		return DATASTORAGE_ERR_NOMEMORYLEFT;
	}
	if (asprintf(&temp_file_path, "%s.temp", file_path) < 1) {
		LOGE(TAG, "Failed to allocate memory for temp file path");
        FREE_MEM(file_path)
		return DATASTORAGE_ERR_NOMEMORYLEFT;
	}
    if ((file = fopen(temp_file_path, "rb")) != NULL) {
        if (remove(temp_file_path) != ESP_OK) {
            LOGI(TAG, "Failed to erase temp file from partition, but continue with original file");
        }
    }
	if (stat(file_path, &file_stat) == 0) {
		*file_status = true;
	}
	else {
		*file_status = false;
	}
    FREE_MEM(temp_file_path);
    FREE_MEM(file_path);

    return DATASTORAGE_OK;
}


uint8_t datastorage_erase_file_from_partition(const char *file_name) {
	char *temp_file_path;
	char *file_path;
    uint8_t rc;

	if (file_name == NULL) {
		LOGE(TAG, "Specified file name not initialized");
		return DATASTORAGE_ERR_GENERAL;
	}
	if (asprintf(&file_path, "/mcufirm/%s", file_name) < 1) {
		LOGE(TAG, "Failed to allocate memory for file path");
		return DATASTORAGE_ERR_NOMEMORYLEFT;
	}
	if (asprintf(&temp_file_path, "%s.temp", file_path) < 1) {
		LOGE(TAG, "Failed to allocate memory for temp file path");
        FREE_MEM(file_path)
		return DATASTORAGE_ERR_NOMEMORYLEFT;
	}
	LOGD(TAG, "Erase temp file (if exists) with file path: %s", temp_file_path);
    if (remove(temp_file_path) != ESP_OK) {
		LOGD(TAG, "Failed to erase temp file from partition, may not exits so continue with original file delete");
    }
	LOGD(TAG, "Erase file with file path: %s", file_path);
	if (remove(file_path) != ESP_OK) {
		LOGE(TAG, "Failed to erase file from partition");
		rc = DATASTORAGE_ERR_FILE_ERASE;
        goto exit;
	}
	LOGI(TAG, "Successfully erased file from partition");

    rc = DATASTORAGE_OK;
    exit:
    FREE_MEM(temp_file_path)
    FREE_MEM(file_path)
	return rc;
}


uint8_t fcopy(char *source_path, char *dest_path) {
    FILE *source = fopen(source_path, "rb");
    if (!source) {
        return DATASTORAGE_ERR_GENERAL;
    }

    FILE *dest = fopen(dest_path, "wb");
    if (!dest) {
        fclose(source);
        return DATASTORAGE_ERR_GENERAL;
    }

    char *buffer = (char *)malloc(sizeof(char) * 4096);
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        fwrite(buffer, 1, bytes, dest);
    }

    FREE_MEM(buffer)
    fclose(source);
    fclose(dest);
    return DATASTORAGE_OK;
}


uint8_t datastorage_write_data_to_file_in_partition(const char *file_name, uint8_t type, uint8_t *data, uint32_t *cnt) {
	char *temp_file_path;
	char *file_path;
	FILE *file;
    uint8_t rc;

	if (file_name == NULL) {
		LOGE(TAG, "Specified file name not initialized");
		return DATASTORAGE_ERR_GENERAL;
	}
	if (type > 3) {
		LOGE(TAG, "Specified invalid write type");
		return DATASTORAGE_ERR_GENERAL;
	}
	if (asprintf(&file_path, "/mcufirm/%s", file_name) < 1) {
		LOGE(TAG, "Failed to allocate memory for file path");
		return DATASTORAGE_ERR_NOMEMORYLEFT;
	}
	if (asprintf(&temp_file_path, "%s.temp", file_path) < 1) {
		LOGE(TAG, "Failed to allocate memory for temp file path");
        FREE_MEM(file_path)
		return DATASTORAGE_ERR_NOMEMORYLEFT;
	}
    remove(temp_file_path);
	LOGD(TAG, "Handle write operation: %hhu for file with file path: %s and temp file path: %s", type, file_path, temp_file_path);
	if (type == DATASTORAGE_WRITE_DATA || type == DATASTORAGE_WRITE_BINARY_DATA) {
		file = fopen(temp_file_path, "wb");
	}
	else if (type == DATASTORAGE_APPEND_DATA || type == DATASTORAGE_APPEND_BINARY_DATA) {
        fcopy(file_path, temp_file_path);
		file = fopen(temp_file_path, "ab");
	}
	else {
		file = NULL;
	}
	if (file == NULL) {
		LOGE(TAG, "Failed to open file for writing");
        rc = DATASTORAGE_ERR_FILE_OPEN;
        goto exit;
	}
	if (fwrite(data, 1, *cnt, file) != *cnt) {
		LOGE(TAG, "Failed to write data to file");
		fclose(file);
        rc = DATASTORAGE_ERR_FILE_WRITE;
        goto exit;
	}
	fclose(file);
    if (rename(temp_file_path, file_path) != 0) {
        LOGE(TAG, "Failed to rename temp file to target file");
        rc = DATASTORAGE_ERR_FILE_WRITE;
        goto exit;
    }
	LOGI(TAG, "Finished file write operation");


    rc = DATASTORAGE_OK;
    exit:
    FREE_MEM(temp_file_path)
    FREE_MEM(file_path)
    return rc;
}


uint8_t datastorage_read_data_from_file_in_partition(const char *file_name, uint8_t type, uint8_t **data, uint32_t *cnt) {
	char *file_path;
    uint8_t rc;
    int32_t ret;
	FILE *file;
	int32_t size;

	if (file_name == NULL) {
		LOGE(TAG, "Specified file name not initialized");
		return DATASTORAGE_ERR_GENERAL;
	}
	if (type > 1) {
		LOGE(TAG, "Specified invalid read type");
		return DATASTORAGE_ERR_GENERAL;
	}
	if (asprintf(&file_path, "/mcufirm/%s", file_name) < 1) {
		LOGE(TAG, "Failed to allocate memory for file path");
		return DATASTORAGE_ERR_NOMEMORYLEFT;
	}
	LOGD(TAG, "Handle read operation: %hhu for file with file path: %s", type, file_path);
	if (type == DATASTORAGE_READ_DATA || type == DATASTORAGE_READ_BINARY_DATA) {
		file = fopen(file_path, "rb");
		if (file == NULL) {
			LOGE(TAG, "Failed to open file for reading");
            rc = DATASTORAGE_ERR_FILE_OPEN;
            goto exit;
		}
		if (*cnt == 0) {
			if (fseek(file, 0, SEEK_END) != 0) {
				LOGE(TAG, "Failed to seek to file end");
				fclose(file);
                rc = DATASTORAGE_ERR_FILE_READ;
                goto exit;
			}
			size = ftell(file);
			LOGD(TAG, "File size: %dB", size);
			if (fseek(file, 0, SEEK_SET) != 0) {
				LOGE(TAG, "Failed to seek to file start");
				fclose(file);
                rc = DATASTORAGE_ERR_FILE_READ;
                goto exit;
			}
			if ((*data = (uint8_t *)malloc(size)) == NULL) {
				LOGE(TAG, "Failed to allocate memory for data");
				fclose(file);
				rc = DATASTORAGE_ERR_NOMEMORYLEFT;
                goto exit;
			}
		}
		else {
			size = *cnt;
		}
		ret = fread(*data, 1, size, file);
		if (ret == EOF) {
			LOGE(TAG, "Failed to read data from file");
			FREE_MEM(*data);
			fclose(file);
			rc = DATASTORAGE_ERR_FILE_READ;
		}
		*cnt = ret;
		fclose(file);
	}
	LOGI(TAG, "Finished file read operation, size: %u", size);

    rc = DATASTORAGE_OK;
    exit:
    FREE_MEM(file_path)
	return rc;
}


uint8_t datastorage_write_chunked_binary_data_to_file_in_partition(const char *fileName, uint32_t offset, uint8_t *data, uint32_t *cnt, bool lastChunk) {
	int32_t ret;

	if (fileName == NULL) {
		LOGE(TAG, "Specified file name not initialized");
		return DATASTORAGE_ERR_GENERAL;
	}

	if (*cnt == 0) {
		LOGE(TAG, "Specified invalid file size for writing");
		return DATASTORAGE_ERR_GENERAL;
	}

	if (globFilePath != NULL) {
		if (strstr(globFilePath, fileName) == NULL) {
			LOGD(TAG, "Entered file name does not match cache values, clear cache");
			FREE_MEM(globFilePath);
			fclose(globFile);
		}
		else if (!(globFile->_flags & __SWR)) {
			LOGD(TAG, "Entered file is currently not opened for writing, clear cache");
			FREE_MEM(globFilePath);
			fclose(globFile);
		}
	}

	if (globFilePath == NULL) {
		LOGD(TAG, "First write chunk, creating global file name cache");
		ret = asprintf(&globFilePath, "/%s", fileName);
		if (ret < 1) {
			LOGE(TAG, "Failed to allocate memory for global file path");
			return DATASTORAGE_ERR_NOMEMORYLEFT;
		}

		LOGD(TAG, "First write chunk, creating global file cache");
		globFile = fopen(globFilePath, "wb");
		if (globFile == NULL) {
			LOGE(TAG, "Failed to open file for writing");
			FREE_MEM(globFilePath);
			return DATASTORAGE_ERR_FILE_OPEN;
		}
	}

	LOGD(TAG, "Handle chunked binary write operation for file with file path: %s at offset: %lu with size: %lu", globFilePath, offset, *cnt);

	if (offset != 0xFFFFFFFF) {
		ret = fseek(globFile, (long)offset, SEEK_SET);
		if (ret != 0) {
			LOGE(TAG, "Failed to seek to position: %u", offset);
			FREE_MEM(globFilePath);
			fclose(globFile);
			return DATASTORAGE_ERR_FILE_WRITE;
		}
	}

	ret = fwrite(data, 1, *cnt, globFile);
	if (ret != *cnt) {
		LOGE(TAG, "Failed to write chunk to file");
		FREE_MEM(globFilePath);
		fclose(globFile);
		return DATASTORAGE_ERR_FILE_WRITE;
	}

	if (lastChunk) {
		LOGI(TAG, "Successfully finished file writing");
		FREE_MEM(globFilePath);
		fclose(globFile);
	}
	else {
		LOGI(TAG, "Successfully wrote chunk to file");
	}

	return DATASTORAGE_OK;
}


uint8_t datastorage_read_chunked_binary_data_from_file_in_partition(const char *fileName, uint32_t offset, uint8_t *data, uint32_t *cnt) {
	int32_t ret;

	if (fileName == NULL) {
		LOGE(TAG, "Specified file name not initialized");
		return DATASTORAGE_ERR_GENERAL;
	}

	if (*cnt == 0) {
		LOGE(TAG, "Specified invalid file size for reading");
		return DATASTORAGE_ERR_GENERAL;
	}

	if (globFilePath != NULL) {
		if (strstr(globFilePath, fileName) == NULL) {
			LOGD(TAG, "Entered file name does not match cache values, clear cache");
			FREE_MEM(globFilePath);
			fclose(globFile);
		}
		else if (!(globFile->_flags & __SRD)) {
			LOGD(TAG, "Entered file is currently not opened for reading, clear cache");
			FREE_MEM(globFilePath);
			fclose(globFile);
		}
	}

	if (globFilePath == NULL) {
		LOGD(TAG, "First read chunk, creating global file name cache");
		ret = asprintf(&globFilePath, "/%s", fileName);
		if (ret < 1) {
			LOGE(TAG, "Failed to allocate memory for global file path");
			return DATASTORAGE_ERR_NOMEMORYLEFT;
		}

		LOGD(TAG, "First read chunk, creating global file cache");
		globFile = fopen(globFilePath, "rb");
		if (globFile == NULL) {
			LOGE(TAG, "Failed to open file for reading");
			FREE_MEM(globFilePath);
			return DATASTORAGE_ERR_FILE_OPEN;
		}
	}

	LOGD(TAG, "Handle chunked binary read operation for file with file path: %s at offset: %u with size: %u", globFilePath, offset, *cnt);

	if (offset != 0xFFFFFFFF) {
		ret = fseek(globFile, (long)offset, SEEK_SET);
		if (ret != 0) {
			LOGE(TAG, "Failed to seek to position: %u", offset);
			FREE_MEM(globFilePath);
			fclose(globFile);
			return DATASTORAGE_ERR_FILE_READ;
		}
	}

	ret = fread(data, 1, *cnt, globFile);
	if (ret != *cnt) {
		*cnt = ret;
		LOGD(TAG, "File read operation finished, clearing global cache");
		FREE_MEM(globFilePath);
		fclose(globFile);
	}

	LOGI(TAG, "Successfully read chunk from file");

	return DATASTORAGE_OK;
}


uint8_t datastorage_clear_chunked_file_cache() {
	if (globFilePath != NULL) {
		LOGD(TAG, "Clearing file path cache");
		FREE_MEM(globFilePath);
	}

	if (globFile != NULL) {
		LOGD(TAG, "Clearing open file cache");
		fclose(globFile);
	}

	LOGI(TAG, "Successfully cleared chunked file handle");

	return DATASTORAGE_OK;
}


uint8_t datastorage_erase_storage_partition() {
	int32_t ret;

	ret = esp_littlefs_format("mcufirm");
	if (ret != ESP_OK) {
		LOGE(TAG, "Failed to erase mcufirm partition, err: %d", ret);
		return DATASTORAGE_ERR_PARTITION;
	}

	return DATASTORAGE_OK;
}


uint8_t datastorage_mount_storage_partition(bool systemMount) {
	esp_vfs_littlefs_conf_t littlefsConf;
	uint32_t total_size;
	uint32_t used_size;
	int32_t ret;

	if (esp_littlefs_mounted("mcufirm")) {
		LOGD(TAG, "Storage partition already mounted, skip mounting");
		return DATASTORAGE_OK;
	}
	LOGD(TAG, "Mount storage partition");

	littlefsConf.base_path = "/mcufirm";
	littlefsConf.partition_label = "mcufirm";
	littlefsConf.format_if_mount_failed = systemMount;
	littlefsConf.read_only = false;
	littlefsConf.dont_mount = false;
	littlefsConf.grow_on_mount = false;
	ret = esp_vfs_littlefs_register(&littlefsConf);
	if (ret != ESP_OK) {
		LOGE(TAG, "Failed to mount storage partition, ret: %d", ret);
		return DATASTORAGE_ERR_GENERAL;
	}
	ret = esp_littlefs_info("mcufirm", (size_t *)&total_size, (size_t *)&used_size);
	if (ret != ESP_OK) {
		LOGW(TAG, "Failed to get current storage partition stats, ret: %d", ret);
	}
	else {
		LOGD(TAG, "Storage partition total size: %uB, used size: %uB", total_size, used_size);
	}
	LOGI(TAG, "Successfully mounted storage partition");

	return DATASTORAGE_OK;
}


uint8_t datastorage_unmount_storage_partition() {
	int32_t ret;

	if (!esp_littlefs_mounted("mcufirm")) {
		LOGD(TAG, "Storage partition not mounted");
		return DATASTORAGE_OK;
	}
	LOGD(TAG, "Unmount storage partition");

	ret = esp_vfs_littlefs_unregister("mcufirm");
	if (ret != ESP_OK) {
		LOGE(TAG, "Failed to unmount storage partition, ret: %d", ret);
		return DATASTORAGE_ERR_GENERAL;
	}
	LOGI(TAG, "Successfully unmounted storage partition");

	return DATASTORAGE_OK;
}
