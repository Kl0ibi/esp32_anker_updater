#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <esp_system.h>
#include <sys/time.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <esp_wifi_default.h>
#include <esp_wifi_types.h>
#include "wifi.h"
#include "system.h"
#include "logging.h"
#include "api.h"
#include "types.h"

ESP_EVENT_DEFINE_BASE(INTERNAL_WIFI_EVENT);


#define SSID ("")
#define PASSWORD ("")


#define WIFI_CONNECT_WAIT                           (pdMS_TO_TICKS(8000))
#define WIFI_DISCONNECT_WAIT                        (pdMS_TO_TICKS(8000))

#define WIFI_WAIT_FOR_IP                            (pdMS_TO_TICKS(4000))

#define WIFI_WAIT_CONNECTED_DURATION                (pdMS_TO_TICKS(180000))
#define WIFI_WAIT_NOT_CONNECTED_DURATION            (pdMS_TO_TICKS(10000))

#define WIFI_CONNECT_FAILED                         (pdMS_TO_TICKS(10000))

#define MAX_ATTEMPTS_OF_RECONNECTS_DURING_ESP_FAIL  (5)


static const char *TAG = "anker_wifi";


enum wifi_handling_events {
	EV_NONE,
	EV_CONNECT_TO_DEFAULT_AP,
    EV_CONNECT_TO_C_ANKER_AP,
};

// WiFi Handling States used for Mealy State Machine
enum wifi_handling_states {
	ST_IDLE,
	ST_CONNECT,
	ST_CHANGE_TO_AP,
};

typedef enum wifi_connection_status {
	CONNECTED,
	DISCONNECTED
} connection_status;


typedef struct {
	bool wifi_sta_active;                   ///< Provides information whether an WIFI Station is connected (true) or not connected (false). 
	bool wifi_ap_active;                    ///< Provides information whether an WIFI AP is open (true) or not (false).
	bool wifi_handler_active;               ///< Is the wifi handler currently running (true) or not running (false).
	bool is_connected;                      ///< Provides information whether an WIFI Station is connected (true) or not connected (false).
	esp_netif_t *sta_netif;                 ///< Pointer to the esp_netif_t structure.
	esp_netif_t *ap_netif;                 ///< Pointer to the esp_netif_t structure.
	EventGroupHandle_t status_bits;         ///< Event group for BIT management.
} wifi_client_t;


static const int CONNECT_BIT = BIT0;                        ///< Request that connection to C-Anker AP should be established
static const int CREATE_AP_BIT = BIT1;                      ///< Request that C-Anker AP should be created
static const int WIFI_CONNECTION_LOST_BIT = BIT2;           ///< Wifi connection got lost
static const int WIFI_DISCONNECTED_BIT = BIT3;              ///< Disconnected with WiFi
static const int WIFI_CONNECTION_PROCESS_DONE_BIT = BIT4;   ///< Wifi connection process done
static const int ACCEPTED_BIT = BIT5;                       ///< Wifi process is finished without an Error
static const int REJECTED_BIT = BIT6;                       ///< Wifi process not finished within time or with an Error

static wifi_client_t *wifi_client = NULL;
static TaskHandle_t htask;
static enum wifi_handling_states cur_handling_state = ST_CONNECT;
static enum wifi_handling_events cur_handling_event = EV_CONNECT_TO_DEFAULT_AP;
static uint64_t wifi_ticks_to_wait = WIFI_WAIT_NOT_CONNECTED_DURATION;
static uint8_t attempts_reconnect = 0;
static bool reconnect = false;
static wifi_ap_record_t cur_wifi;
static wifi_sta_list_t cur_ap;
wifi_config_t config = {};

static char ssid[32];
static char password[32];


void set_timeouts(connection_status status) {
	if (status == CONNECTED) {
		wifi_ticks_to_wait = WIFI_WAIT_CONNECTED_DURATION;
	}
	else {
		wifi_ticks_to_wait = WIFI_WAIT_NOT_CONNECTED_DURATION;
	}
}


static uint8_t change_config(bool sta, wifi_handling_events cur_event) {
    uint8_t rc;

    if (sta) {
        if (esp_wifi_stop() != ESP_OK) {    // Do Handling like described in https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html under Configuration Phase
            LOGE(TAG, "Error at stopping wifi");
            return WIFI_ERROR;
        }
        if (cur_event == EV_CONNECT_TO_DEFAULT_AP || ssid[0] == '\0') {
            memset(&config, 0, sizeof(config));
            strcpy((char *)config.sta.ssid, SSID);
            strcpy((char *)config.sta.password, PASSWORD);
        }
        else {
            memset(&config, 0, sizeof(config));
            strcpy((char *)config.sta.ssid, ssid);
            strcpy((char *)config.sta.password, password);
        }
        config.ap.ssid_hidden = true;
        if ((rc = esp_wifi_set_mode(api_get_device_type() == DEVICE_TYPE_C_ANKER ? WIFI_MODE_APSTA : WIFI_MODE_STA)) != ESP_OK) {
            LOGE(TAG, "Error in esp_wifi_set_mode! RC=%d", rc);
            esp_wifi_start();
            return WIFI_ERROR;
        }
        if ((rc = esp_wifi_set_config(WIFI_IF_STA, &config)) != ESP_OK) {
            LOGE(TAG,  "Error in esp_wifi_set_config! for STA.");
            esp_wifi_start();
            return WIFI_ERROR;
        }
        if ((rc = esp_wifi_start()) != ESP_OK) {
            LOGE(TAG, "Error at starting wifi rc=%d", rc);
            return WIFI_ERROR;
        }
        wifi_client->wifi_sta_active = true;
        wifi_client->wifi_ap_active = false;
    }
    else {
        if (ssid[0] == '\0' || password[0] == '\0') {
            LOGE(TAG, "Received invalid ARGS for AP");
            return WIFI_ERROR;
        }
        if (esp_wifi_stop() != ESP_OK) {
            LOGE(TAG, "Error at stopping wifi for AP");
            return WIFI_ERROR;
        }

        if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) {
            LOGE(TAG, "Error in esp_wifi_set_mode! RC=%d", rc);
            esp_wifi_start();
            return WIFI_ERROR;
        }
        strncpy((char *)config.ap.ssid, ssid, sizeof(ssid));
        strncpy((char *)config.ap.password, password, sizeof(password));
        config.ap.max_connection = MAX_P_ANKER_TO_UPDATE;
        config.ap.channel = 1;
        config.ap.ssid_hidden = true;
        config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        if ((rc = esp_wifi_set_config(WIFI_IF_AP, &config)) != ESP_OK) {
            LOGE(TAG,  "Error in esp_wifi_set_config rc=%d!  for AP. Connect to default again...", rc);
            esp_wifi_start();
            return WIFI_ERROR;
        }
        if ((rc = esp_wifi_start()) != ESP_OK) {
            LOGE(TAG, "Error at starting wifi for AP rc=%d. Connect to default again...", rc);
            return WIFI_ERROR;
        }
        wifi_client->wifi_ap_active = true;
        wifi_client->wifi_sta_active = false;
    }

    return WIFI_OK;
}


static void wifi_handling_task(void *pvParameters) {
	uint8_t rc;
	EventBits_t ux_bits;
	bool still_connected;
	xEventGroupClearBits(wifi_client->status_bits, ACCEPTED_BIT | WIFI_DISCONNECTED_BIT |  WIFI_CONNECTION_LOST_BIT | WIFI_CONNECTION_PROCESS_DONE_BIT | REJECTED_BIT | CONNECT_BIT | CREATE_AP_BIT);

	while (wifi_client->wifi_handler_active) {
		switch (cur_handling_state) {
            case ST_IDLE:
                ux_bits = xEventGroupWaitBits(wifi_client->status_bits, WIFI_CONNECTION_LOST_BIT | CONNECT_BIT | CREATE_AP_BIT, pdTRUE, pdFALSE, wifi_ticks_to_wait);
                if ((ux_bits & WIFI_CONNECTION_LOST_BIT) != 0) {
                    // Connection lost perform active scan, select and connect
                    LOGI(TAG, "Wifi connection got lost");
                    set_timeouts(DISCONNECTED);
                    cur_handling_event = EV_CONNECT_TO_DEFAULT_AP;
                    cur_handling_state = ST_CONNECT;
                    break;
                }
                else if ((ux_bits & CONNECT_BIT) != 0) {
                    LOGI(TAG, "Request to connect to C-ANKER AP");
                    cur_handling_event = EV_CONNECT_TO_C_ANKER_AP;
                    cur_handling_state = ST_CONNECT;
                    break;
                }
                else if ((ux_bits & CREATE_AP_BIT) != 0) {
                    LOGI(TAG, "Create C-Anker AP");
                    cur_handling_event = EV_NONE;
                    cur_handling_state = ST_CHANGE_TO_AP;
                    break;
                }
                else {
                    LOGI(TAG, "Timeout exceeded");
                    if (wifi_client->wifi_sta_active) {
                        if (esp_wifi_sta_get_ap_info(&cur_wifi) == ESP_OK) {
                            set_timeouts(CONNECTED);
                            cur_handling_event = EV_NONE;
                            cur_handling_state = ST_IDLE;
                            break;
                        }
                        else {
                            LOGD(TAG, "Wifi not connected");
                            cur_handling_state = ST_CONNECT;
                            cur_handling_event = EV_CONNECT_TO_DEFAULT_AP;
                        }
                    }
                    else if (wifi_client->wifi_ap_active) {
                        if (esp_wifi_ap_get_sta_list(&cur_ap) == ESP_OK) {
                            LOGI(TAG, "AP active with currently %u connected clients", cur_ap.num);
                            set_timeouts(CONNECTED);
                            cur_handling_event = EV_NONE;
                            cur_handling_state = ST_IDLE;
                            break;
                        }
                        else { // Here adapt logic what should happen
                            LOGD(TAG, "Wifi AP not up! Connect to default AP");
                            set_timeouts(DISCONNECTED);
                            cur_handling_state = ST_CONNECT;
                            cur_handling_event = EV_CONNECT_TO_DEFAULT_AP;
                        }
                    }
                    else {
                        LOGW(TAG, "Wifi not connected or AP, connect to default AP");
                        set_timeouts(DISCONNECTED);
                        cur_handling_state = ST_CONNECT;
                        cur_handling_event = EV_CONNECT_TO_DEFAULT_AP;
                    }
                }
                break;

            case ST_CHANGE_TO_AP:
                if (change_config(false, cur_handling_event) != WIFI_OK) {
                    cur_handling_event = EV_CONNECT_TO_DEFAULT_AP;
                    cur_handling_state = ST_CONNECT;
                    set_timeouts(DISCONNECTED);
                    xEventGroupSetBits(wifi_client->status_bits, REJECTED_BIT);
                }
                else {
                    LOGI(TAG, "Sucessfully created AP with ssid: %s", ssid);
                    cur_handling_event = EV_NONE;
                    cur_handling_state = ST_IDLE;
                    xEventGroupSetBits(wifi_client->status_bits, ACCEPTED_BIT);
                }
                break;

            case ST_CONNECT:
                if (change_config(true, cur_handling_event) != WIFI_OK) {
                    set_timeouts(DISCONNECTED);
                    if (cur_handling_event == EV_CONNECT_TO_C_ANKER_AP) {
                        xEventGroupSetBits(wifi_client->status_bits, REJECTED_BIT);
                    }
                    cur_handling_event = EV_NONE;
                    cur_handling_state = ST_IDLE;
                    set_timeouts(DISCONNECTED);
                    break;
                }
                retry: // If there is a Disconnect Reason caused by an internal ESP error or by a router failure try to reconnect up to five times.
                xEventGroupClearBits(wifi_client->status_bits, WIFI_CONNECTION_PROCESS_DONE_BIT | WIFI_DISCONNECTED_BIT | REJECTED_BIT | ACCEPTED_BIT);
                LOGD(TAG, "Try to connect");
                rc = esp_wifi_connect();
                ux_bits = xEventGroupWaitBits(wifi_client->status_bits, WIFI_CONNECTION_PROCESS_DONE_BIT | WIFI_DISCONNECTED_BIT, pdTRUE, pdFALSE, WIFI_CONNECT_WAIT);
                if ((ux_bits & WIFI_CONNECTION_PROCESS_DONE_BIT) != 0) {
                    LOGI(TAG, "Connected with AP");
                    if (cur_handling_event == EV_CONNECT_TO_C_ANKER_AP) {
                        xEventGroupSetBits(wifi_client->status_bits, ACCEPTED_BIT);
                    }
                    attempts_reconnect = 0;
					reconnect = false;
                    cur_handling_event = EV_NONE;
                    cur_handling_state = ST_IDLE;
                    set_timeouts(CONNECTED);
                    break;
                }
                else { // Disconnected Bit got set or Connection-Timeout exceeded.
                    if ((ux_bits & WIFI_DISCONNECTED_BIT) != 0) {
                        if (reconnect) {
                            LOGI(TAG, "Retry connection because of internal ESP-Scan failure!");
                            goto retry;
                        }
                    }
                    else {
                        LOGE(TAG, "Error Connection-Timeout exceeded!");
						reconnect = false;
                    }
                    if (rc != ESP_OK) {
                        LOGE(TAG, "Error in wifi_connect");
                    }
                    set_timeouts(DISCONNECTED);
                }
                set_timeouts(DISCONNECTED);
                cur_handling_event = EV_NONE;
                cur_handling_state = ST_IDLE;
                if (cur_handling_event == EV_CONNECT_TO_C_ANKER_AP) {
                    xEventGroupSetBits(wifi_client->status_bits, REJECTED_BIT);
                }
                break;
        }
		vTaskDelay(10);
	}

	LOGI(TAG, "Wifi station inactive. Deleting WIFI_HANDLING_TASK");
	wifi_client->wifi_handler_active = false;
	vTaskDelete(NULL);
}


static uint8_t wifi_start() {
	uint8_t rc;
	BaseType_t tr;

	if (!wifi_client) {
		return WIFI_ERROR;
	}
	ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_NULL) );
	ESP_ERROR_CHECK( esp_wifi_start() );

	if (!wifi_client->wifi_handler_active) {
        tr = xTaskCreatePinnedToCore(wifi_handling_task, "wifi_handling_task", 3072, NULL, 6, &htask, PRO_CPU_NUM);
        if (tr != pdTRUE) {
            return WIFI_ERROR;
        }
	    wifi_client->wifi_handler_active = true;
	}

	return WIFI_OK;
}


static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		LOGI(TAG, "WIFI_EVENT_STA_START");
	}
	else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
		LOGI(TAG, "WIFI_EVENT_STA_STOP");
        wifi_client->wifi_sta_active = false;
	}
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
		LOGI(TAG, "WIFI_EVENT_AP_START");
	}
	else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
		LOGI(TAG, "WIFI_EVENT_AP_STOP");
        wifi_client->wifi_ap_active = false;
	}
	else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
		LOGI(TAG, "WIFI_EVENT_SCAN_DONE");
	}
	else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		LOGI(TAG, "WIFI_EVENT_STA_DISCONNECTED");
		xEventGroupSetBits(wifi_client->status_bits, WIFI_DISCONNECTED_BIT);
		wifi_client->is_connected = false;
		wifi_event_sta_disconnected_t *dr = (wifi_event_sta_disconnected_t *)event_data;
		wifi_err_reason_t reason = (wifi_err_reason_t)dr->reason;
		LOGW(TAG, "Disconnected. Reason: %d", reason);
		// If there is a Disconnect Reason caused by an internal ESP error or by a router failure try to reconnect up to five times.
		// Look up "Rainy Day" Scenarios and Error Codes: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html#esp32-wi-fi-api-error-code
		/*
		 *  Code                                    Value           Retry
		 *  WIFI_REASON_UNSPECIFIED                 1               Yes
		 *  WIFI_REASON_AUTH_EXPIRE                 2               Yes
		 *  WIFI_REASON_AUTH_LEAVE                  3               Yes
		 *  WIFI_REASON_ASSOC_EXPIRE                4               Yes
		 *  WIFI_REASON_ASSOC_TOOMANY               5               Yes
		 *  WIFI_REASON_NOT_AUTHED                  6               Yes
		 *  WIFI_REASON_NOT_ASSOCED                 7               Yes
		 *  WIFI_REASON_ASSOC_LEAVE                 8               No
		 *  WIFI_REASON_ASSOC_NOT_AUTHED            9               Yes
		 *  WIFI_REASON_DISASSOC_PWRCAP_BAD         10              No
		 *  WIFI_REASON_DISASSOC_SUPCHAN_BAD        11              No
		 *  WIFI_REASON_IE_INVALID                  13              No
		 *  WIFI_REASON_MIC_FAILURE                 14              Yes
		 *  WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT      15              No
		 *  WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT    16              Yes
		 *  WIFI_REASON_IE_IN_4WAY_DIFFERS          17              No
		 *  WIFI_REASON_GROUP_CIPHER_INVALID        18              Yes
		 *  WIFI_REASON_PAIRWISE_CIPHER_INVALID     19              Yes
		 *  WIFI_REASON_AKMP_INVALID                20              No
		 *  WIFI_REASON_UNSUPP_RSN_IE_VERSION       21              No
		 *  WIFI_REASON_INVALID_RSN_IE_CAP          22              No
		 *  WIFI_REASON_802_1X_AUTH_FAILED          23              Yes
		 *  WIFI_REASON_CIPHER_SUITE_REJECTED       24              Yes
		 *  WIFI_REASON_INVALID_PMKID               53              No
		 *  WIFI_REASON_BEACON_TIMEOUT              200             No
		 *  WIFI_REASON_NO_AP_FOUND                 201             Yes
		 *  WIFI_REASON_AUTH_FAIL                   202             Yes
		 *  WIFI_REASON_ASSOC_FAIL                  203             Yes
		 *  WIFI_REASON_HANDSHAKE_TIMEOUT           204             Yes
		 *  WIFI_REASON_CONNECTION_FAIL             205             Yes
		 *  WIFI_REASON_AP_TSF_RESET                206             Yes
		 *  WIFI_REASON_ROAMING                     207             No
		 */
		if (reason != WIFI_REASON_ASSOC_LEAVE && reason != WIFI_REASON_IE_INVALID && reason != WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT &&
			reason != WIFI_REASON_IE_IN_4WAY_DIFFERS && reason != WIFI_REASON_AKMP_INVALID && reason != WIFI_REASON_UNSUPP_RSN_IE_VERSION &&
			reason != WIFI_REASON_INVALID_RSN_IE_CAP && reason != WIFI_REASON_INVALID_PMKID && reason != WIFI_REASON_BEACON_TIMEOUT &&
			reason != WIFI_REASON_DISASSOC_PWRCAP_BAD && reason != WIFI_REASON_DISASSOC_SUPCHAN_BAD && reason != WIFI_REASON_ROAMING) {
			if (attempts_reconnect >= MAX_ATTEMPTS_OF_RECONNECTS_DURING_ESP_FAIL) {
				reconnect = false;
			}
			else {
                reconnect = true;
                attempts_reconnect++;
			}
		}
		else if (reason != WIFI_REASON_ASSOC_LEAVE) { // Disconnect by State-Machine (manual forced Disconnect by esp_wifi_disconnect())
			reconnect = false;
			attempts_reconnect = 0;
		}
	}
	else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
		LOGI(TAG, "WIFI_EVENT_STA_CONNECTED");
		//xEventGroupSetBits(wifi_client->status_bits, WIFI_CONNECTION_PROCESS_DONE_BIT);
	}
	else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		LOGI(TAG, "IP_EVENT_STA_GOT_IP");
		wifi_client->is_connected = true;
		//xEventGroupSetBits(wifi_client->status_bits, WIFI_GOT_IP);
		xEventGroupSetBits(wifi_client->status_bits, WIFI_CONNECTION_PROCESS_DONE_BIT);
		ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
		LOGI(TAG, "got ip:%s", ip4addr_ntoa((const ip4_addr_t *)&event->ip_info.ip));
	}
}


bool wifi_get_connection_status() {
	return wifi_client->is_connected;
}


// if ssid is null connec to default AP
uint8_t wifi_connect_to_ap(const char *req_ssid, const char *req_password) {
	uint8_t ux_bits;

    if (req_ssid == NULL) {
        ssid[0] = '\0';
    }
    else {
        strncpy((char *)ssid, req_ssid, AP_SSID_SIZE);
    }   
    if (req_password == NULL) {
        password[0] = '\0';
    }
    else {
        strncpy((char *)password, req_password, AP_PASSWORD_SIZE);
    }
	xEventGroupClearBits(wifi_client->status_bits,ACCEPTED_BIT | REJECTED_BIT);
	xEventGroupSetBits(wifi_client->status_bits, CONNECT_BIT);
	ux_bits = xEventGroupWaitBits(wifi_client->status_bits, ACCEPTED_BIT | REJECTED_BIT, pdTRUE, pdFALSE, WIFI_CONNECT_FAILED);
	if ((ux_bits & ACCEPTED_BIT) != 0) {
        LOGI(TAG, "Successfully connected to AP", ssid, password);
		return WIFI_OK;
	}

    LOGE(TAG, "Failed to connect to AP");
	return WIFI_ERROR;
}


uint8_t wifi_create_ap(const char *req_ssid, const char *req_password) {
	uint8_t ux_bits;

    if (req_ssid == NULL || req_password == NULL || req_ssid[0] == '\0' || req_password[0] == '\0') {
	    return WIFI_ERROR;
    }

    strncpy((char *)ssid, req_ssid, AP_SSID_SIZE);
    strncpy((char *)password, req_password, AP_PASSWORD_SIZE);
	xEventGroupClearBits(wifi_client->status_bits,ACCEPTED_BIT | REJECTED_BIT);
	xEventGroupSetBits(wifi_client->status_bits, CREATE_AP_BIT);
	ux_bits = xEventGroupWaitBits(wifi_client->status_bits, ACCEPTED_BIT | REJECTED_BIT, pdTRUE, pdFALSE, WIFI_CONNECT_FAILED);
	if ((ux_bits & ACCEPTED_BIT) != 0) {
		return WIFI_OK;
	}

	return WIFI_ERROR;
}



uint8_t wifi_init() {
	uint8_t rc;

	if (wifi_client) {
		return WIFI_ALREADY_INITIALIZED;
	}
	if ((wifi_client = (wifi_client_t *)calloc(1, sizeof(wifi_client_t))) == NULL) {
		return WIFI_ERROR;
	}
	wifi_client->status_bits = xEventGroupCreate();

	wifi_client->sta_netif = esp_netif_create_default_wifi_sta();
	wifi_client->ap_netif = esp_netif_create_default_wifi_ap();
	wifi_client->is_connected = false;

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

	wifi_country_t ccconf = {
			.cc = "00", // worldwide setting, for regional use .cc = "AT"
			.schan = 1,
			.nchan = 13,
			.policy = WIFI_COUNTRY_POLICY_MANUAL
	};
	if ((rc = esp_wifi_set_country(&ccconf)) != WIFI_OK) {
		LOGE(TAG, "Error during setup of wifi country code! RC=%i", rc);
	}
	if ((rc = wifi_start()) != WIFI_OK) {
		LOGE(TAG, "Error during nrgwifi_start()! RC=%i", rc);
	}

	return rc;
}
