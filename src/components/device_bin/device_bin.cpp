#include <stdio.h>
#include <string.h>
#include <nvs_flash.h>
#include "device_bin.h"
#include "api.h"


#define VERSION ("2.0.0")

#define PARTITION_LABEL "device"
#define NVS_KEY_SERIAL_NUMBER "sn"
#define NVS_KEY_PRODUCT_TYPE "prty"
#define NVS_KEY_HB_SECURITY_TOKEN "hbst"


typedef struct {
    serial_number_t sn;
    uint8_t device_type;
    hb_security_token_t hb_security_token;
} device_bin_nvs_content_t;


static device_bin_nvs_content_t *nvs_content;
static software_version_t version;


const serial_number_t *device_bin_get_serial_number() {
    return &nvs_content->sn;
}


const software_version_t *device_bin_get_version() {
    return &version;
}


const device_type_t device_bin_get_device_type() {
    return (device_type_t)nvs_content->device_type;
}


const hb_security_token_t *device_bin_get_hb_security_token() {
    return &nvs_content->hb_security_token;
}


static uint8_t devie_bin_read_device_info(device_bin_nvs_content_t *content) {
	int rc;
	nvs_handle_t my_handle;
	esp_err_t err;

    if (nvs_open_from_partition((const char*)PARTITION_LABEL, (const char*)PARTITION_LABEL, NVS_READONLY, &my_handle) != ESP_OK) {
        return DEVICE_BIN_ERROR;
    }
    size_t len = SERIAL_SIZE;
	if (nvs_get_str(my_handle, (const char *)NVS_KEY_SERIAL_NUMBER, content->sn.serial, &len) != ESP_OK || len != SERIAL_SIZE) {
        return DEVICE_BIN_ERROR;
    }
	if (strlen(strupr(content->sn.serial)) <= 0) {
        return DEVICE_BIN_ERROR;
	}
    if (nvs_get_u8(my_handle, (const char *)NVS_KEY_PRODUCT_TYPE, &content->device_type) != ESP_OK || content->device_type == DEVICE_TYPE_UNKNOWN) {
        return DEVICE_BIN_ERROR;
    }
    if (content->device_type == DEVICE_TYPE_C_ANKER) {
        len = HB_TOKEN_SIZE;
        if (nvs_get_str(my_handle, (const char *)NVS_KEY_HB_SECURITY_TOKEN, content->hb_security_token.token, &len) != ESP_OK || len != HB_TOKEN_SIZE) {
            return DEVICE_BIN_ERROR;
        }
    }
    else {
        content->hb_security_token.token[0] = '\0';
    }
    nvs_close(my_handle);

	return DEVICE_BIN_OK;
}


uint8_t device_bin_init() {
	if (nvs_flash_init_partition(PARTITION_LABEL) != ESP_OK) {
		return DEVICE_BIN_ERROR;
	}
	if ((nvs_content = (device_bin_nvs_content_t*)calloc(1, sizeof(device_bin_nvs_content_t))) == NULL) {
		return DEVICE_BIN_ERROR;
	}
	if (devie_bin_read_device_info(nvs_content) != DEVICE_BIN_OK) {
		return DEVICE_BIN_ERROR;
	}
    strncpy(version.version, VERSION, sizeof(version.version));
    version.version[strlen(VERSION)] = '\0';

	return DEVICE_BIN_OK;
}
