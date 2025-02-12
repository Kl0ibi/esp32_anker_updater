#ifndef API_H
#define API_H

#include <stdint.h>
#include "types.h"
#include "../src/components/timecontroller/include/timecontroller_types.h"


#define API_OK (0)
#define API_ERROR (1)


typedef enum {
  DEVICE_TYPE_UNKNOWN = 0,
  DEVICE_TYPE_C_ANKER = 1,
  DEVICE_TYPE_P_ANKER = 2,
} device_type_t; //TODO: move to types


const serial_number_t *api_get_serial_number();


const software_version_t *api_get_version();


const device_type_t api_get_device_type();


const hb_security_token_t *api_get_hb_security_token();


int api_get_timestamp_without_timezone(timestamp_status_t *timestamp_status, bool force_timesave);


void api_restart_anker();


uint8_t api_init();


#endif
