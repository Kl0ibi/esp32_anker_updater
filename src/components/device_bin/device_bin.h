#ifndef DEVICE_BIN_H
#define DEVICE_BIN_H


#include <stdint.h>
#include "api.h"
#include "types.h"

#define DEVICE_BIN_OK (0)
#define DEVICE_BIN_ERROR (1)



const serial_number_t *device_bin_get_serial_number();


const software_version_t *device_bin_get_version();


const device_type_t device_bin_get_device_type();


const hb_security_token_t *device_bin_get_hb_security_token();


uint8_t device_bin_init();


#endif
