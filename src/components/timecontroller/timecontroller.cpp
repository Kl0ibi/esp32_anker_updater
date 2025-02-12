#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <esp_sntp.h>
#include "logging.h"
#include "timecontroller.h"
#include "system.h"
#include "core/timecontroller_core.h"


ESP_EVENT_DEFINE_BASE(NRG_TIME_CONTROLLER_EVENT);


// region Type definitions
typedef struct timecontroller_client_handle {
	timecontroller_init_params_t config;
} timecontroller_client_handle_t;
// endregion


// KEYS For NVSTORAGEMAN
#define NVSM_KEY_TIMECONTROLLER_TIME_ZONE               "_tctimzn"
#define TIMECONTROLLER_TIMEZONE_ENV                     "TZ"


static const char TAG[] = "timecontroller";
static timecontroller_client_handle_t *client;


int timecontroller_get_timezone(timcon_timezone_t *timezone) {
	int rc = TIMCON_OK;

	if (!client) {
		return TIMCON_CLIENT_ERR;
	}

	if ((rc = timecontroller_core_get_timezone(timezone)) != TIMCON_OK) {
		LOGE(TAG, "Timezone %i could not be retrieved. RC=%i", *timezone, rc);
		return TIMCON_ERR_GENERAL;
	}

	return TIMCON_OK;
}


int timecontroller_set_timezone(timcon_timezone_t *timezone) {
	int rc;

	if (!client) {
		return TIMCON_CLIENT_ERR;
	}

	if ((rc = timecontroller_core_set_timezone(timezone)) != TIMCON_OK) {
		LOGE(TAG, "Timezone %i could not be set. RC=%i", *timezone, rc);
		return TIMCON_ERR_GENERAL;
	}

	return TIMCON_OK;
}


timestamp_sync_type timecontroller_get_ts_sync_type() {
	return timecontroller_core_get_ts_sync_type();
}


int timecontroller_get_timezone_offset(int *offset) {
	if (!client) {
		return TIMCON_CLIENT_ERR;
	}

	return timecontroller_core_get_timezone_offset(offset);
}


int timecontroller_get_timestamp_with_timezone(timestamp_status_t *ts) {
	int rc = TIMCON_OK, offset = 0;
	time_t time_with_offset;
	struct timeval current_time = {0};

	if (!client) {
		return TIMCON_CLIENT_ERR;
	}

	// Get current time without timezone
	gettimeofday(&current_time, NULL);

	// Get current timezone offset
	rc = timecontroller_core_get_timezone_offset(&offset);
	if (rc != TIMCON_OK) {
		LOGW(TAG, "Timezone offset could not be obtained! RC=%i", rc);
	}

	time_with_offset = current_time.tv_sec + offset;
	LOGD(TAG, "Current time of day without timezone: %ld", time_with_offset);

	ts->timestamp = time_with_offset;
	ts->sync_status = timecontroller_core_get_ts_sync_status();


	return TIMCON_OK;
}


int timecontroller_get_timestamp_without_timezone(timestamp_status_t *ts, bool force_timesave) {
	struct timeval current_time = {0};

	if (!client) {
		return TIMCON_CLIENT_ERR;
	}

	// Retrieve the current system time
	gettimeofday(&current_time, NULL);
	LOGD(TAG, "Current time of day without timezone: %ld", current_time.tv_sec);

	// Fill timestamp struct with data
	ts->timestamp = current_time.tv_sec;
	ts->sync_status = timecontroller_core_get_ts_sync_status();

	if (force_timesave) {
		// Necessary for better validation of timestamps.
		timecontroller_core_save_ts_to_storage(&current_time.tv_sec);
	}

	return TIMCON_OK;
}


int timecontroller_update_last_timestamp() {
	struct timeval current_time = {0};

	gettimeofday(&current_time, NULL);
	if (timecontroller_core_save_ts_to_storage(&current_time.tv_sec) != TIMCON_OK) {
		LOGE(TAG, "Timestamp could not be saved to storage");
		return TIMCON_ERR_GENERAL;
	}

	return TIMCON_OK;
}


int timecontroller_update_invalid_timestamp(time_t *timestamp) {
	return timecontroller_core_update_invalid_timestamp(timestamp);
}


int timecontroller_sync_timestamps(timestamp_status_t *timestamps_array, int array_size) {
	if (!client) {
		return TIMCON_CLIENT_ERR;
	}

	return timecontroller_core_sync_timestamps(timestamps_array, array_size);
}


int timecontroller_handle_rtc_update(const time_t *timestamp, timestamp_sync_type sync_type) {
	int rc = TIMCON_OK;

	if (!client) {
		return TIMCON_CLIENT_ERR;
	}
	if (timestamp == NULL) {
		return TIMCON_INVALID_VALUE_ERR;
	}
	if ((rc = timecontroller_core_handle_rtc_update(timestamp, sync_type)) != TIMCON_OK) {
		LOGE(TAG, "Time could not be handled! RC=%i", rc);
	}

	return rc;
}


// Overwritten function of SNTP (Function is called when SNTP receives a realtime timestamp)
void sntp_sync_time(struct timeval *tv) {
	LOGI(TAG, "Time update %ld provided by SNTP.", tv->tv_sec);
	timecontroller_handle_rtc_update(&tv->tv_sec, TIMESTAMP_SYNC_STATUS_SNTP);
}


void timecontroller_start_sntp() {
	if (esp_sntp_enabled() == 0) {
		LOGI(TAG, "Starting SNTP ...");
		// Set operation mode
		esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
		// Set servername
		esp_sntp_setservername(0, SNTP_ADDRESS);

		// Start SNTP
		esp_sntp_init();
		LOGD(TAG, "SNTP started!");
	}
}


void timecontroller_stop_sntp() {
	if (esp_sntp_enabled() == 1) {
		LOGI(TAG, "Stopping SNTP ...");
		esp_sntp_stop();
		LOGD(TAG, "SNTP stopped!");
	}
}


static int init_timecontroller_core() {
	int rc;

	timecontroller_core_init_params_t core_init_params = {
			.nvs_set_tz = client->config.nvs_set_tz,
			.nvs_get_tz = client->config.nvs_get_tz,
			.get_timestamp = timecontroller_get_timestamp_without_timezone,
			.nvs_save_ts = client->config.nvs_save_ts,
			.nvs_load_ts = client->config.nvs_load_ts,
			.rtc_updated_cb = client->config.rtc_updated_cb,
			.tz_updated_cb = client->config.tz_update_cb,
	};
	if ((rc = timecontroller_core_init(&core_init_params)) != TIMCON_OK) {
		LOGE(TAG, "Unable to start timecontroller-core! RC=%d", rc);
		return TIMCON_ERR_GENERAL;
	}

	return TIMCON_OK;
}


int timecontroller_init(timecontroller_init_params_t *init_params) {
	if (init_params == NULL) {
		return TIMCON_ERR_GENERAL;
	}

	if (client) {
		LOGE(TAG, "Timecontroller already initialized!");
		return TIMCON_CLIENT_ERR;
	}

	if (!init_params->nvs_load_ts || !init_params->nvs_save_ts ||
	    !init_params->nvs_set_tz || !init_params->nvs_get_tz) {
		return TIMCON_ERR_GENERAL;
	}

	if ((client = (timecontroller_client_handle_t*)calloc(1, sizeof(timecontroller_client_handle_t))) == NULL) {
		return TIMCON_MEMORY_ERR;
	}
	memcpy(&client->config, init_params, sizeof(timecontroller_init_params_t));

	return init_timecontroller_core();
}


int timecontroller_destroy() {
	int rc;

	FREE_MEM(client);

	timecontroller_stop_sntp();

	if ((rc = timecontroller_core_destroy()) != TIMCON_OK) {
		LOGE(TAG, "TimeControllerCore could not be destroyed! RC=%i", rc);
		return rc;
	}

	return TIMCON_OK;
}
