#include <api.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include "esp_event.h"
#include "logging.h"
#include "../device_bin/device_bin.h"
#include "../wifi/wifi.h"
#include "../nvstorageman/nvstorageman.h"
#include "../timecontroller/timecontroller.h"
#include "../updater/updater.h"
#include "../datastorage/datastorage.h"
#include "types.h"


static const char *TAG = "anker_api";

static bool internet_connected = false;


bool api_get_wifi_status() {
	return wifi_get_connection_status();
}


static void internet_connection_state_changed(esp_event_base_t event_base, int32_t event_id) {
	bool is_wifi_netif_up = api_get_wifi_status();

    if (is_wifi_netif_up) {
        timecontroller_start_sntp();
        if (updater_start(api_get_device_type()) != UPDATER_OK) {
            LOGE(TAG, "Error starting updater");
        }
        internet_connected = true;
    }
    else {
        timecontroller_stop_sntp();
        if (updater_stop(api_get_device_type()) != UPDATER_OK) {
            LOGE(TAG, "Error stopping updater");
        }
        internet_connected = false;
    }
}


static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
		LOGI(TAG, "Wifi AP connection established");
	}
	else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		LOGI(TAG, "Got IP address from wifi ap");
		internet_connection_state_changed(event_base, event_id);
	}
	else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		LOGI(TAG, "Wifi AP connection lost");
		internet_connection_state_changed(event_base, event_id);
	}
	else if (event_base == INTERNAL_WIFI_EVENT && event_id == WIFI_EVENT_WIFI_STOP) {
		LOGI(TAG, "Wifi interface stopped");
	    internet_connected = false;
	}
	else {
		LOGW(TAG, "Unused Event triggered: EVENTBase: %s, EVENTID: %ld", event_base, event_id);
	}
}


static int time_updated_to_rtc_cb() {
	LOGD(TAG, "Time updated to RTC callback triggered.");
	return API_OK;
}


static int timezone_updated_cb() {
    LOGI(TAG, "EVENT: Timezone got updated");
	return API_OK;
}


const serial_number_t *api_get_serial_number() {
    return device_bin_get_serial_number();
}


const software_version_t *api_get_version() {
    return device_bin_get_version();
}


const device_type_t api_get_device_type() {
    return device_bin_get_device_type();
}


const hb_security_token_t *api_get_hb_security_token() {
    return device_bin_get_hb_security_token();
}


int api_get_timestamp_without_timezone(timestamp_status_t *timestamp_status, bool force_timesave) {
	return timecontroller_get_timestamp_without_timezone(timestamp_status, force_timesave);
}


void api_restart_anker() {
	timecontroller_update_last_timestamp();
	esp_restart();
}


static uint8_t timecontroller_module_init() {
	uint8_t rc;
	timecontroller_init_params_t init_params = {
			.nvs_set_tz = nvsm_set_u8,
			.nvs_get_tz = nvsm_get_u8,
			.nvs_save_ts = nvsm_set_u32,
			.nvs_load_ts = nvsm_get_u32,
			.rtc_updated_cb = time_updated_to_rtc_cb,
			.tz_update_cb = timezone_updated_cb,
	};
	rc = timecontroller_init(&init_params);
	if (rc != TIMCON_OK) {
		LOGE(TAG, "Error init Timecontroller-Module! RC=%i", rc);
		return API_ERROR;
	}

	return API_OK;
}


uint8_t api_init() {
    esp_err_t err;

	ESP_ERROR_CHECK(esp_event_loop_create_default());
	ESP_ERROR_CHECK(esp_netif_init());
    
	if ((err = nvsm_init()) != ESP_OK) {
		LOGE(TAG, "Error init nvs storage! RC=%d", err);
		abort();
	}
    if (device_bin_init() != DEVICE_BIN_OK) {
		LOGE(TAG, "Error during init of device_bin init");
        abort();
    }
    if (timecontroller_module_init() != API_OK) {
        LOGE(TAG, "Eror during init of timecontroller");
    }
    if (wifi_init() != WIFI_OK) {
        LOGE(TAG, "Error init wifi!");
        return API_ERROR;
    }
	if (datastorage_mount_storage_partition(true) != DATASTORAGE_OK) {
		LOGE(TAG, "Error mounting storage partition! RC=");
	}
    if (updater_init(api_get_device_type()) != UPDATER_OK) {
        LOGE(TAG, "Error init hawkbit!");
    }

	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(INTERNAL_WIFI_EVENT, WIFI_EVENT_WIFI_STOP, &wifi_event_handler, NULL));

    return API_OK;
}
