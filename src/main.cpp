#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <api.h>
#include "logging.h"
#include "system.h"

static const char *TAG = "main";


void setup() {
    bool startup_ok = false;
    device_type_t device_type;
    serial_number_t serial_number;
    software_version_t version;
    
    // logs
    logging_set_global_log_level(LOG_LEVEL_DEBUG);
    Serial.println("Anker starting...");

    // system tests
    /*if (api_get_device_type(&device_type) != API_OK || device_type == DEVICE_TYPE_UNKNOWN) {
        LOGE(TAG, "Error while getting device type");
        goto exit;
    }
    if (api_get_serial_number(&serial_number) != API_OK) {
        LOGE(TAG, "Error while getting device type");
        goto exit;
    }
    if (api_get_version(&version) != API_OK) {
        LOGE(TAG, "Error while getting device type");
        goto exit;
    }
    LOGI(TAG, "Type: %s, SN: %s, SW: %s", device_type == DEVICE_TYPE_C_ANKER ? "C-Anker" : "P-Anker", serial_number.serial, version.version);
    */
    // init 
    api_init();

    // start

    startup_ok = true;
    exit:
    if (!startup_ok) { // enter error state or restart :/
        esp_restart();
    }   
    Serial.println("Anker started");
}


void loop() {
    Serial.println("just doing general anker stuff...");
    delay(10000);
}
