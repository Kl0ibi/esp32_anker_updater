#ifndef HAWKBIT_CLIENT_H
#define HAWKBIT_CLIENT_H

#include <stdint.h>

#define HAWKBITCLIENT_OK 0
#define HAWKBITCLIENT_ERR_GENERAL 1
#define NRGVRBSTORAGE_ERR_NOTINIT 2
#define HAWKBITCLIENT_ERR_NOMEMLEFT 3
#define HAWKBITCLIENT_ERR_HEADERS 4
#define HAWKBITCLIENT_ERR_HTTPCLIENT 5
#define HAWKBITCLIENT_ERR_REQUEST 6
#define HAWKBITCLIENT_ERR_JSONPARSING 7
#define HAWKBITCLIENT_ERR_RETRIES_EXCEEDED 8
#define HAWKBITCLIENT_ERR_DOWNLOADING 9
#define HAWKBITCLIENT_ERR_ESPOTA 10


typedef enum controller_update_status {
	CONTROLLER_UPDATE_STATUS_UNKNOWN = 0,
	CONTROLLER_UPDATE_STATUS_UPDATE_AVAILABLE = 1,
	CONTROLLER_UPDATE_STATUS_ALREADY_INSTALLED = 2,
	CONTROLLER_UPDATE_STATUS_DOWNLOAD_SUCCESSFUL = 3,
    CONTROLLER_UPDATE_STATUS_DOWNLOAD_FAILED = 4,
    CONTROLLER_UPDATE_STATUS_DOWNLOAD_INVALID = 5,
	CONTROLLER_UPDATE_STATUS_INSTALL_SUCCESSFUL = 6,
	CONTROLLER_UPDATE_STATUS_INSTALL_FAILED = 7,
	CONTROLLER_UPDATE_STATUS_HANDLE_P_ANKER_UPDATE = 8,
	CONTROLLER_UPDATE_STATUS_HANDLED_P_ANKER_UPDATE = 9,
} controller_update_status_t;


typedef struct time_info {
	bool time_valid;
	uint32_t time_value;
} time_info_t;

typedef struct controller_update_info {
	controller_update_status_t status;
	time_info_t time_info;
    uint8_t cnt_to_update;
    uint8_t cnt_joined;
    uint8_t cnt_updated;
} controller_update_info_t;

typedef struct current_update_info {
	uint32_t update_id;
	controller_update_info_t anker;
} current_update_info_t;


bool hawkbit_client_handle_is_running();


uint8_t hawkbit_client_trigger_event();


uint8_t hawkbit_client_start();


uint8_t hawkbit_client_stop();


uint8_t hawkbit_client_init();


#endif
