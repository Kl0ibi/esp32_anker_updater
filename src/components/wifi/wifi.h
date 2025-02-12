#include <stdint.h>
#include <esp_event_base.h>

#define WIFI_OK (0)
#define WIFI_ERROR (1)
#define WIFI_ALREADY_INITIALIZED (2)

ESP_EVENT_DECLARE_BASE(INTERNAL_WIFI_EVENT);


enum {
	WIFI_EVENT_WIFI_STOP,
};


bool wifi_get_connection_status();


uint8_t wifi_init();


uint8_t wifi_connect_to_ap(const char *req_ssid, const char *req_password);


uint8_t wifi_create_ap(const char *req_ssid, const char *req_password);
