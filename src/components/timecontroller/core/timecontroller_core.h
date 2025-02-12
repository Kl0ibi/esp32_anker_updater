#ifndef TIMECONTROLLER_CORE_H
#define TIMECONTROLLER_CORE_H


#include "../include/timecontroller_types.h"


typedef struct timecontroller_core_init_params {
	timecontroller_nvs_set_tz nvs_set_tz;
	timecontroller_nvs_get_tz nvs_get_tz;
	timecontroller_get_ts get_timestamp;
	timecontroller_nvs_set_ts nvs_save_ts;
	timecontroller_nvs_get_ts nvs_load_ts;
	timecontroller_rtc_updated_cb rtc_updated_cb;
	timecontroller_timezone_updated_cb tz_updated_cb;
} timecontroller_core_init_params_t;


int timecontroller_core_get_timezone(timcon_timezone_t *tz);


int timecontroller_core_set_timezone(const timcon_timezone_t *tz);


int timecontroller_core_get_timezone_offset(int *offset);


int timecontroller_core_save_ts_to_storage(const time_t *ts);


int timecontroller_core_load_ts_from_storage(time_t *ts);


int timecontroller_core_sync_timestamps(timestamp_status_t *ts_arr, int array_size);


int timecontroller_core_update_invalid_timestamp(time_t *timestamp);


int timecontroller_core_handle_rtc_update(const time_t *timestamp, timestamp_sync_type sync_type);


bool timecontroller_core_get_ts_sync_status();


timestamp_sync_type timecontroller_core_get_ts_sync_type();


int timecontroller_core_init(timecontroller_core_init_params_t *init_params);


int timecontroller_core_destroy();


#endif
