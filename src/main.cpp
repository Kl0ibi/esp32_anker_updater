#include <Arduino.h>
#include "api.h"
#include "logging.h"
#include "system.h"

static const char *TAG = "main";


void setup() {
    bool startup_ok;
    device_type_t device_type;
    serial_number_t serial_number;
    software_version_t version;
    
    // logs
    Serial.begin(115200);
    logging_set_global_log_level(LOG_LEVEL_DEBUG);
    Serial.println("Anker starting...");

    // init 
    if (api_init() != API_OK) {
        startup_ok = false;
        goto exit;
    }

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
