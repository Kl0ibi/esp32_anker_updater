#include <time.h>
#include <string.h>
#include <esp_ota_ops.h>
#include "logging.h"
#include "components/timecontroller/include/timecontroller_types.h"
#include "system.h"
#include "timecontroller_core.h"


#define NVSM_KEY_TIMECONTROLLER_TIME_ZONE               "_tctimzn"          /*!< NVS-Key used to save/load timezone */
#define NVSM_KEY_TIMECONTROLLER_CURRENT_TIMESTAMP       "_tc_localts"       /*!< NVS-Key used to save/load timestamp */
#define TIMECONTROLLER_TIMEZONE_ENV                     "TZ"


static const char TAG[] = "timecontroller_core";

static timecontroller_core_init_params_t *params = NULL;  				/*!< Params and callbacks provided during init. */
static timcon_timezone_t cache_timezone = (timcon_timezone_t)0;             				/*!< Cached Timezone value. */
static bool ts_sync_status = false;                                         /*!< Current RTC sync status. */
static timestamp_sync_type ts_sync_type = TIMESTAMP_SYNC_STATUS_UNSYNCED; 	/*!< Current RTC sync type. */
static time_t ts_before_rtc = 0;                            				/*!< Timestamp before sync to RTC. */
static time_t ts_rtc_sync = 0;                              				/*!< Timestamp of RTC at sync. */
static time_t timestamp_init = 0;                           				/*!< Saved timestamp retrieved at init. */


// region Timezone related functionality
static int get_current_timezone(timcon_timezone_t *tz) {
	// Use cached timezone if available
	if (cache_timezone != 0) {
		memcpy(tz, &cache_timezone, sizeof(timcon_timezone_t));
		return TIMCON_OK;
	}

	// Load timezone from storage
	if (params->nvs_get_tz(NVSM_KEY_TIMECONTROLLER_TIME_ZONE, (uint8_t *)tz) < 0) {
		*tz = timezone_GMT_0;
		return TIMCON_OK;
	}
	cache_timezone = *tz;

	return TIMCON_OK;
}


static long calc_dstbias() {
	struct timeval tv;
	struct tm local_time_info = { 0 };
	gettimeofday(&tv, NULL);
	localtime_r(&tv.tv_sec, &local_time_info);
	if (local_time_info.tm_isdst) {
		timcon_timezone_t tz;
		get_current_timezone(&tz);
		if (tz == timezone_GMT_p1030_p11) {
			LOGI(TAG, "DST is active, and has an offset of -1800s");
			return -1800;
		}
		if (tz == timezone_GMT_0_p02) {
			LOGI(TAG, "DST is active, and has an offset of -7200s");
			return -7200;
		}
		LOGI(TAG, "DST is active, and has an offset of -3600s");
		return -3600;
	}
	LOGI(TAG, "DST is not active");

	return 0;
}


static int verify_timezone(const timcon_timezone_t *tz) {
	if (tz == NULL) {
		return TIMCON_POINTER_ERR;
	}

	if (*tz == timezone_UNKNOWN_TIMEZONE_TYPE) {
		return TIMCON_INVALID_VALUE_ERR;
	}

	if (*tz > TIMECON_TIMEZONE_MAX || *tz < TIMECON_TIMEZONE_MIN) {
		return TIMCON_OUT_OF_RANGE_ERR;
	}

	/*if (TIMEZONE_STR[*tz] == 0) {
		return TIMCON_ERR_GENERAL;
	}*/

	return TIMCON_OK;
}


static int update_system_timezone(const timcon_timezone_t *tz) {
	int rc;
	// For documentation see: https://docs.espressif.com/projects/esp-idf/en/v4.1/api-reference/system/system_time.html?highlight=timezone#timezones

	rc = verify_timezone(tz);
	if (rc != TIMCON_OK) {
		LOGW(TAG, "Invalid timezone provided!");
		return TIMCON_ERR_GENERAL;
	}
	/*if (setenv(TIMECONTROLLER_TIMEZONE_ENV, TIMEZONE_STR[*tz], 1) < 0) {
		return TIMCON_ERR_GENERAL;
	}*/
	tzset();

	return TIMCON_OK;
}


static int set_current_timezone(const timcon_timezone_t *tz) {
	int rc = TIMCON_OK;

	rc = verify_timezone(tz);
	if (rc != TIMCON_OK) {
		return rc;
	}
	if (params->nvs_set_tz(NVSM_KEY_TIMECONTROLLER_TIME_ZONE, *tz) < 0) {
		return TIMCON_NVS_KEY_NOT_FOUND;
	}
	cache_timezone = *tz;

	return TIMCON_OK;
}
//endregion


// region RTC-functionality
/**
 * @brief Update the system time.
 *
 * @param timestamp: Time to update system time with.
 * @return
 *      - TIMCON_OK (Success)
 *      - TIMCON_ERR_GENERAL
 * */
static int update_system_time(const time_t *timestamp) {
	int rc;
	struct timeval system_time = {.tv_sec = *timestamp};
	rc = settimeofday(&system_time, NULL);
	if (rc != 0) {
		return TIMCON_ERR_GENERAL;
	}
	params->nvs_save_ts(NVSM_KEY_TIMECONTROLLER_CURRENT_TIMESTAMP, (uint32_t)system_time.tv_sec);
	LOGI(TAG, "Changed system time successfully to %ld", system_time.tv_sec);

	return TIMCON_OK;
}


static int handle_change_to_rtc(const time_t *timestamp) {
	int rc = TIMCON_OK;

	timestamp_status_t *ts = (timestamp_status_t *)calloc(1, sizeof(timestamp_status_t));
	if (ts == NULL) {
		LOGW(TAG, "Memory in handle_change_to_rtc could not be allocated!");
		return TIMCON_MEMORY_ERR;
	}

	/* Get current timestamp */
	if (params->get_timestamp(ts, true) != TIMCON_OK) {
		rc = TIMCON_ERR_GENERAL;
		goto exit;
	}
	else {
		ts_before_rtc = ts->timestamp;
	}

	rc = update_system_time(timestamp);
	if (rc != TIMCON_OK) {
		LOGE(TAG, "System time could not be updated! RC=%i", rc);
		goto exit;
	}

	// Used for calculation of synced ts
	ts_rtc_sync = *timestamp;
	LOGD(TAG, "Timestamp before sync to RTC: %ld", ts_before_rtc);
	LOGD(TAG, "Timestamp at sync to RTC: %ld", ts_rtc_sync);

	exit:
	FREE_MEM(ts);

	return rc;
}
// endregion


// region Syncing functionality
static int sync_timestamps(timestamp_status_t *ts_arr, const int *array_size, bool sync_status,
                           const time_t *timestamp_rtc_sync, const time_t *timestamp_before_rtc) {
	if (!ts_arr || !array_size || !timestamp_rtc_sync || !timestamp_before_rtc) {
		return TIMCON_INVALID_VALUE_ERR;
	}

	if (sync_status) {
		// Update unsynced_timestamps
		for (int i = 0; i < *array_size; i++) {
			// Check if timestamp is already synced and larger than 0 (in this case it should not be synced)
			if (!ts_arr[i].sync_status && ts_arr[i].timestamp > 0) {
				// Calculate realtime timestamp & overwrite value
				ts_arr[i].timestamp = *timestamp_rtc_sync - (*timestamp_before_rtc - ts_arr[i].timestamp);
				ts_arr[i].sync_status = true;
			}
		}
		return TIMCON_OK;
	}
	return TIMCON_NOT_SYNCED_ERR;
}
// endregion


bool timecontroller_core_get_ts_sync_status() {
	return ts_sync_status;
}


timestamp_sync_type timecontroller_core_get_ts_sync_type() {
	return ts_sync_type;
}


int timecontroller_core_get_timezone(timcon_timezone_t *tz) {
	return get_current_timezone(tz);
}


int timecontroller_core_set_timezone(const timcon_timezone_t *tz) {
	int rc = TIMCON_OK;

	rc = set_current_timezone(tz);
	if (rc != TIMCON_OK) {
		return rc;
	}

	rc = update_system_timezone(tz);
	params->tz_updated_cb();
	return rc;
}


int timecontroller_core_get_timezone_offset(int *offset) {
	// _timezone is defined in time.h and stores the offset of the set timezone
	*offset = (int)_timezone + calc_dstbias(); // since the _timezone value does not have the dst offset included add it
	*offset *= (-1);

	return TIMCON_OK;
}


int timecontroller_core_handle_rtc_update(const time_t *timestamp, timestamp_sync_type sync_type) {
	int rc;
	struct tm time_info = {0};
	time_t validation_timestamp;

	strptime(esp_ota_get_app_description()->date, "%b %d %Y", &time_info);
	validation_timestamp = mktime_without_timezone(&time_info);
	LOGD(TAG, "build ts: %lu - %s / requested sync type: %hhu", validation_timestamp, esp_ota_get_app_description()->date, sync_type);

	if (*timestamp < validation_timestamp) {
		LOGE(TAG, "Requested timestamp is smaller than validation timestamp");
		return TIMCON_INVALID_VALUE_ERR;
	}
	if (!timecontroller_core_get_ts_sync_status()) {
		timestamp_status_t temp_ts = {0};

		if (params->get_timestamp(&temp_ts, true) != TIMCON_OK) {
			return TIMCON_ERR_GENERAL;
		}
		rc = handle_change_to_rtc(timestamp);
		if (rc != TIMCON_OK) {
			LOGE(TAG, "Handle of RTC failed! RC=%i", rc);
			update_system_time(&temp_ts.timestamp); // Use previous timestamp
			return rc;
		}
		ts_sync_status = true;
		ts_sync_type = sync_type;
		params->rtc_updated_cb();
	}
	else if (sync_type >= ts_sync_type) {
		update_system_time(timestamp);
		ts_sync_status = true;
		ts_sync_type = sync_type;

		return TIMCON_OK;
	}
	else {
		LOGI(TAG, "System time already got synced by higher prioritized source!");
	}

	return TIMCON_OK;
}


int timecontroller_core_save_ts_to_storage(const time_t *ts) {
	if (ts == NULL) {
		return TIMCON_INVALID_VALUE_ERR;
	}

	if (params->nvs_save_ts(NVSM_KEY_TIMECONTROLLER_CURRENT_TIMESTAMP, (uint32_t)*ts) < 0) {
		return TIMCON_ERR_GENERAL;
	}

	return TIMCON_OK;
}


int timecontroller_core_load_ts_from_storage(time_t *ts) {
	if (ts == NULL) {
		return TIMCON_INVALID_VALUE_ERR;
	}

	if (params->nvs_load_ts(NVSM_KEY_TIMECONTROLLER_CURRENT_TIMESTAMP, (uint32_t *)ts) < 0) {
		return TIMCON_ERR_GENERAL;
	}

	return TIMCON_OK;
}


int timecontroller_core_sync_timestamps(timestamp_status_t *ts_arr, int array_size) {
	return sync_timestamps(ts_arr, &array_size, timecontroller_core_get_ts_sync_status(),
	                       &ts_rtc_sync, &ts_before_rtc);
}


static int init_timezone() {
	timcon_timezone_t timezone = (timcon_timezone_t)0;
	if (get_current_timezone(&timezone) != TIMCON_OK) {
		LOGW(TAG, "No Timezone saved yet!");
	}
	else {
		set_current_timezone(&timezone);
		update_system_timezone(&timezone);
	}

	return TIMCON_OK;
}


static int init_timestamp() {
	int rc;

	if (timecontroller_core_load_ts_from_storage(&timestamp_init) != TIMCON_OK) {
		struct tm time_info = {0};
		LOGW(TAG, "No timestamp saved yet! using build time");
		strptime(esp_ota_get_app_description()->date, "%b %d %Y", &time_info);
		timestamp_init = mktime_without_timezone(&time_info);
		LOGD(TAG, "build ts: %lu - %s", timestamp_init, esp_ota_get_app_description()->date);
	}
	if ((rc = update_system_time(&timestamp_init)) != TIMCON_OK) {
		LOGE(TAG, "System time could not be updated during timestamp init! RC=%i", rc);
		return rc;
	}
	LOGD(TAG, "Timestamp found. Initialized with %ld", timestamp_init);

	return TIMCON_OK;
}


int timecontroller_core_update_invalid_timestamp(time_t *timestamp) {
	int rc;

	if ((rc = update_system_time(timestamp)) != TIMCON_OK) {
		LOGE(TAG, "System time could not be updated during timestamp init! RC=%i", rc);
		return rc;
	}
	if ((rc = timecontroller_core_save_ts_to_storage( timestamp)) != TIMCON_OK) {
		LOGE(TAG, "Timestamp could not be saved to storage! RC=%i", rc);
		return rc;
	}
	LOGD(TAG, "Updated timetamp to %lu", *timestamp);

	return TIMCON_OK;
}


int timecontroller_core_init(timecontroller_core_init_params_t *init_params) {
	int rc = TIMCON_OK;

	if (init_params == NULL) {
		return TIMCON_ERR_GENERAL;
	}

	if (!init_params->rtc_updated_cb || !init_params->nvs_get_tz || !init_params->nvs_set_tz ||
	    !init_params->nvs_save_ts || !init_params->nvs_load_ts || !init_params->get_timestamp ||
	    !init_params->tz_updated_cb) {
		return TIMCON_INVALID_VALUE_ERR;
	}

	if ((params = (timecontroller_core_init_params_t *)calloc(1, sizeof(timecontroller_core_init_params_t))) == NULL) {
		LOGE(TAG, "Memory in timecontroller_core_init could not be allocated!");
		return TIMCON_MEMORY_ERR;
	}
	memcpy(params, init_params, sizeof(timecontroller_core_init_params_t));

	if ((rc = init_timezone()) != TIMCON_OK) {
		LOGW(TAG, "Timezone could not be initialized! RC=%i", rc);
		return rc;
	}

	if ((rc = init_timestamp()) != TIMCON_OK) {
		LOGW(TAG, "Timestamp could not be initialized! RC=%i", rc);
	}

	return rc;
}


int timecontroller_core_destroy() {
	cache_timezone = (timcon_timezone_t)0;
	ts_before_rtc = 0;
	ts_rtc_sync = 0;
	ts_sync_status = false;
	ts_sync_type = TIMESTAMP_SYNC_STATUS_UNSYNCED;

	if (params) {
		free(params);
		params = 0;
	}
	return TIMCON_OK;
}
