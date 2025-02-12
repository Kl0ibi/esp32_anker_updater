#ifndef TIMECONTROLLER_H
#define TIMECONTROLLER_H

#include <sys/types.h>
#include "esp_event.h"
#include "include/timecontroller_types.h"


ESP_EVENT_DECLARE_BASE(NRG_TIME_CONTROLLER_EVENT);


typedef struct timecontroller_init_params {
	timecontroller_nvs_set_tz nvs_set_tz;
	timecontroller_nvs_get_tz nvs_get_tz;
	timecontroller_nvs_set_ts nvs_save_ts;
	timecontroller_nvs_get_ts nvs_load_ts;
	timecontroller_rtc_updated_cb rtc_updated_cb;
	timecontroller_timezone_updated_cb tz_update_cb;
} timecontroller_init_params_t;


int timecontroller_get_timezone(timcon_timezone_t *timezone);


int timecontroller_set_timezone(timcon_timezone_t *timezone);


timestamp_sync_type timecontroller_get_ts_sync_type();


int timecontroller_get_timezone_offset(int *offset);


int timecontroller_get_timestamp_with_timezone(timestamp_status_t *ts);


int timecontroller_get_timestamp_without_timezone(timestamp_status_t *ts, bool force_timesave);


int timecontroller_update_last_timestamp();


int timecontroller_update_invalid_timestamp(time_t *timestamp);


int timecontroller_sync_timestamps(timestamp_status_t *timestamps_array, int array_size);


int timecontroller_handle_rtc_update(const time_t *timestamp, timestamp_sync_type sync_type);


void timecontroller_start_sntp();


void timecontroller_stop_sntp();


int timecontroller_init(timecontroller_init_params_t *init_params);


int timecontroller_destroy();


#endif
