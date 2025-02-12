#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_http_client.h>
#include <cJSON.h>
#include <mbedtls/sha1.h>
#include <esp_ota_ops.h>
#include "logging.h"
#include "api.h"
#include "system.h"
#include "hawkbit_client.h"
#include "../../datastorage/datastorage.h"
#include "../updater.h"


static const char *TAG = "hawkbit_client";


#define UPDATER_FIRMWARE_FILENAME_UPDATE_STATUS "update.stat"

#define UPDATE_ENDPOINT_URL "192.168.8.10:8080"
#define UPDATE_ENDPOINT_PORT 8080
#define HAWKBIT_TENANT "DEFAULT"
#define HAWKBIT_CLIENT_MAX_URL_SIZE 137


#define TIME_MS_UNTIL_NEXT_POLL_DEFAULT 180000
#define TIME_MS_UNTIL_NEXT_POLL_DEFAULT_WIFI 3600000
#define TIME_MS_UNTIL_NEXT_POLL_MIN 10000
#define TIME_MS_UNTIL_NEXT_POLL_MAX 604800000
#define TIME_MS_UNTIL_NEXT_INSTALL_RETRY 60000

#define HAWKBIT_GET_TIMEOUT 5000
#define HAWKBIT_POST_TIMEOUT 5000
#define HAWKBIT_PUT_TIMEOUT 5000
#define HAWKBIT_DOWNLOAD_TIMEOUT 15000
#define HAWKBIT_DOWNLOAD_MODULE_CHUNK_SIZE 10000
#define HAWKBIT_DOWNLOAD_CONTROLLER_CHUNK_SIZE 8192

#define DOWNLOAD_RETRIES_ANKER 3

#define FIRMWARE_IDENTIFIER_ANKER "anker"

#define FIRMWARE_NAME_ANKER "firmware.bin"

#define FIRMWARE_MAX_SIZE_ANKER 3145728


typedef enum hawkbit_task_run_state {
	HAWKBIT_TASK_RUN_STATE_INACTIVE,
	HAWKBIT_TASK_RUN_STATE_ACTIVE,
	HAWKBIT_TASK_RUN_STATE_BLOCKED,
	HAWKBIT_TASK_RUN_STATE_WAIT
} hawkbit_task_run_state_t;

typedef enum hawkbit_client_bits {
	BIT_STOP = BIT0,
	BIT_STOPPED = BIT1,
	BIT_CP_STANDBY = BIT2,
	BIT_REPOLL = BIT3
} hawkbit_client_bits_t;

typedef enum hawkbit_client_state {
	HAWKBIT_CLIENT_STATE_IDLE,
	HAWKBIT_CLIENT_STATE_POLL,
	HAWKBIT_CLIENT_STATE_DOWNLOAD,
	HAWKBIT_CLIENT_STATE_INSTALL,
	HAWKBIT_CLIENT_STATE_CHECK,
	HAWKBIT_CLIENT_STATE_INFORM,
	HAWKBIT_CLIENT_STATE_STOP
} hawkbit_client_state_t;

typedef struct hawkbit_client_base_content {
	uint32_t time_until_next_poll;
	uint32_t time_until_next_poll_sim;
	char deployment_url[HAWKBIT_CLIENT_MAX_URL_SIZE];
} hawkbit_client_base_content_t;

typedef enum hawkbit_client_update_type {
	HAWKBIT_CLIENT_UPDATE_TYPE_UNKNOWN,
	HAWKBIT_CLIENT_UPDATE_TYPE_FORCED,
	HAWKBIT_CLIENT_UPDATE_TYPE_SOFT,
	HAWKBIT_CLIENT_UPDATE_TYPE_SKIP
} hawkbit_client_update_type_t;

typedef enum hawkbit_client_module_type {
	HAWKBIT_CLIENT_MODULE_TYPE_UNKNOWN,
	HAWKBIT_CLIENT_MODULE_TYPE_ANKER,
} hawkbit_client_module_type_t;

typedef enum hawkbit_client_finish_type {
	HAWKBIT_CLIENT_FINISH_TYPE_UNKNOWN,
	HAWKBIT_CLIENT_FINISH_TYPE_NONE,
	HAWKBIT_CLIENT_FINISH_TYPE_SUCCESS,
	HAWKBIT_CLIENT_FINISH_TYPE_FAILURE
} hawkbit_client_finish_type_t;

typedef enum hawkbit_client_execution_type {
	HAWKBIT_CLIENT_EXECUTION_TYPE_UNKNOWN,
	HAWKBIT_CLIENT_EXECUTION_TYPE_CLOSED,
	HAWKBIT_CLIENT_EXECUTION_TYPE_PROCEEDING,
	HAWKBIT_CLIENT_EXECUTION_TYPE_CANCELED,
	HAWKBIT_CLIENT_EXECUTION_TYPE_SCHEDULED,
	HAWKBIT_CLIENT_EXECUTION_TYPE_REJECTED,
	HAWKBIT_CLIENT_EXECUTION_TYPE_RESUMED
} hawkbit_client_execution_type_t;

typedef struct hawkbit_client_module_content {
	char version[24];
	uint32_t artifact_size;
	char hash_sha1[40];
	char download_url[HAWKBIT_CLIENT_MAX_URL_SIZE];
} hawkbit_client_module_content_t;

typedef struct hawkbit_client_deployment {
	uint32_t id;
	hawkbit_client_update_type_t download;
	hawkbit_client_update_type_t update;
	hawkbit_client_module_content_t anker;
} hawkbit_client_deployment_t;

typedef struct hawkbit_deployment_feedback {
	uint32_t id;
	hawkbit_client_finish_type_t finish_type;
	hawkbit_client_execution_type_t execution_type;
	char *feedback_message_details;
} hawkbit_deployment_feedback_t;


hawkbit_task_run_state_t taskRunState = HAWKBIT_TASK_RUN_STATE_INACTIVE;
bool taskHandleRunning = false;
EventGroupHandle_t taskStatusBits;


bool statusMessageSend = false;


void str_time_to_int(const char *str_time, uint32_t *time) {
	uint8_t h, m, s;

	sscanf(str_time, "%hhu:%hhu:%hhu", &h, &m, &s);

	*time = 0;
	*time += (uint32_t)h * 60 * 60 * 1000;
	*time += (uint32_t)m * 60 * 1000;
	*time += (uint32_t)s * 1000;
}


hawkbit_client_update_type_t update_type_str_to_enum(const char *type) {
	if (strcmp(type, "forced") == 0) {
		return HAWKBIT_CLIENT_UPDATE_TYPE_FORCED;
	}
	else if (strcmp(type, "attempt") == 0) {
		return HAWKBIT_CLIENT_UPDATE_TYPE_SOFT;
	}
	else if (strcmp(type, "skip") == 0) {
		return HAWKBIT_CLIENT_UPDATE_TYPE_SKIP;
	}
	else {
		return HAWKBIT_CLIENT_UPDATE_TYPE_UNKNOWN;
	}
}


hawkbit_client_module_type_t module_type_str_to_enum(const char *type) {
	if (strcmp(type, FIRMWARE_IDENTIFIER_ANKER) == 0) {
		return HAWKBIT_CLIENT_MODULE_TYPE_ANKER;
	}
	else {
		return HAWKBIT_CLIENT_MODULE_TYPE_UNKNOWN;
	}
}


const char *finish_type_to_str(hawkbit_client_finish_type_t finish_type) {
	switch (finish_type) {
		case HAWKBIT_CLIENT_FINISH_TYPE_SUCCESS:
			return "success";
		case HAWKBIT_CLIENT_FINISH_TYPE_FAILURE:
			return "failure";
		default:
			return "none";
	}
}


const char *execution_type_to_str(hawkbit_client_execution_type_t execution_type) {
	switch (execution_type) {
		case HAWKBIT_CLIENT_EXECUTION_TYPE_PROCEEDING:
			return "proceeding";
		case HAWKBIT_CLIENT_EXECUTION_TYPE_CANCELED:
			return "canceled";
		case HAWKBIT_CLIENT_EXECUTION_TYPE_SCHEDULED:
			return "scheduled";
		case HAWKBIT_CLIENT_EXECUTION_TYPE_REJECTED:
			return "rejected";
		case HAWKBIT_CLIENT_EXECUTION_TYPE_RESUMED:
			return "resumed";
		default:
			return "closed";
	}
}


const char *update_status_to_str(controller_update_status_t update_status) {
	switch (update_status) {
		case CONTROLLER_UPDATE_STATUS_ALREADY_INSTALLED:
			return "already installed";
		case CONTROLLER_UPDATE_STATUS_DOWNLOAD_SUCCESSFUL:
			return "download success";
		case CONTROLLER_UPDATE_STATUS_INSTALL_SUCCESSFUL:
			return "install success";
		case CONTROLLER_UPDATE_STATUS_DOWNLOAD_FAILED:
			return "download failed";
		case CONTROLLER_UPDATE_STATUS_DOWNLOAD_INVALID:
			return "download invalid";
		case CONTROLLER_UPDATE_STATUS_INSTALL_FAILED:
			return "install failed";
        case CONTROLLER_UPDATE_STATUS_HANDLE_P_ANKER_UPDATE:
            return "start p-anker update";
        case CONTROLLER_UPDATE_STATUS_HANDLED_P_ANKER_UPDATE:
            return "handled p-anker update";
		default:
			return "none";
	}
}


uint8_t json_parse_actions_to_do(uint8_t *data, hawkbit_client_base_content_t *content) {
	cJSON *json;
	cJSON *config;
	cJSON *polling;
	cJSON *sleep;
	cJSON *sleep_sim;
	cJSON *links;
	cJSON *deployment_base;
	cJSON *href;

	content->time_until_next_poll = 0;
	content->time_until_next_poll_sim = 0;
	content->deployment_url[0] = '\0';

	json = cJSON_Parse((const char *)data);
	if (json == NULL) {
		LOGE(TAG, "Failed to parse json response data");
		return HAWKBITCLIENT_ERR_JSONPARSING;
	}

	config = cJSON_GetObjectItemCaseSensitive(json, "config");
	if (cJSON_IsObject(config)) {
		polling = cJSON_GetObjectItemCaseSensitive(config, "polling");
		if (cJSON_IsObject(polling)) {
			sleep = cJSON_GetObjectItemCaseSensitive(polling, "sleep");
			if (cJSON_IsString(sleep) && sleep->valuestring != NULL) {
				str_time_to_int(sleep->valuestring, &content->time_until_next_poll);
			}
			sleep_sim = cJSON_GetObjectItemCaseSensitive(polling, "sleep_sim");
			if (cJSON_IsString(sleep_sim) && sleep_sim->valuestring != NULL) {
				str_time_to_int(sleep_sim->valuestring, &content->time_until_next_poll_sim);
			}
		}
	}
	links = cJSON_GetObjectItemCaseSensitive(json, "_links");
	if (cJSON_IsObject(links)) {
		deployment_base = cJSON_GetObjectItemCaseSensitive(links, "deploymentBase");
		if (cJSON_IsObject(deployment_base)) {
			href = cJSON_GetObjectItemCaseSensitive(deployment_base, "href");
			if (cJSON_IsString(href) && href->valuestring != NULL) {
				strncpy(content->deployment_url, href->valuestring, sizeof(content->deployment_url));
			}
		}
	}

	cJSON_Delete(json);
	return HAWKBITCLIENT_OK;
}


uint8_t json_parse_artifact(cJSON *artifact, hawkbit_client_module_content_t *content) {
	cJSON *hashes;
	cJSON *sha1;
	cJSON *size;
	cJSON *links;
	cJSON *download_http;
	cJSON *href;

	content->artifact_size = 0;
	content->hash_sha1[0] = '\0';
	content->download_url[0] = '\0';

	hashes = cJSON_GetObjectItemCaseSensitive(artifact, "hashes");
	if (cJSON_IsObject(hashes)) {
		sha1 = cJSON_GetObjectItemCaseSensitive(hashes, "sha1");
		if (cJSON_IsString(sha1) && sha1->valuestring != NULL) {
			memcpy(content->hash_sha1, sha1->valuestring, sizeof(content->hash_sha1));
		}
	}
	size = cJSON_GetObjectItemCaseSensitive(artifact, "size");
	content->artifact_size = size->valueint;
	links = cJSON_GetObjectItemCaseSensitive(artifact, "_links");
	if (cJSON_IsObject(links)) {
		download_http = cJSON_GetObjectItemCaseSensitive(links, "download-http");
		if (cJSON_IsObject(download_http)) {
			href = cJSON_GetObjectItemCaseSensitive(download_http, "href");
			if (cJSON_IsString(href) && href->valuestring != NULL) {
				strncpy(content->download_url, href->valuestring, sizeof(content->download_url));
			}
		}
	}

	return HAWKBITCLIENT_OK;
}


uint8_t json_parse_deployment(uint8_t *data, hawkbit_client_deployment_t *deployment_data) {
	cJSON *json;
	cJSON *id;
	cJSON *deployment;
	cJSON *download;
	cJSON *update;
	cJSON *chunks;
	cJSON *module;
	cJSON *identifier;
	cJSON *version;
	cJSON *artifacts;
	hawkbit_client_module_type_t module_type;
	cJSON *artifact;

	deployment_data->id = 0;
	deployment_data->download = HAWKBIT_CLIENT_UPDATE_TYPE_UNKNOWN;
	deployment_data->update = HAWKBIT_CLIENT_UPDATE_TYPE_UNKNOWN;
	deployment_data->anker.version[0] = '\0';
	deployment_data->anker.artifact_size = 0;
	deployment_data->anker.hash_sha1[0] = '\0';
	deployment_data->anker.download_url[0] = '\0';

	json = cJSON_Parse((const char *)data);
	if (json == NULL) {
		LOGE(TAG, "Failed to parse response json");
		return HAWKBITCLIENT_ERR_JSONPARSING;
	}

	id = cJSON_GetObjectItemCaseSensitive(json, "id");
	if (cJSON_IsString(id) && id->valuestring != NULL) {
		deployment_data->id = atoi(id->valuestring);
	}
	deployment = cJSON_GetObjectItemCaseSensitive(json, "deployment");
	if (cJSON_IsObject(deployment)) {
		download = cJSON_GetObjectItemCaseSensitive(deployment, "download");
		if (cJSON_IsString(download) && download->valuestring != NULL) {
			deployment_data->download = update_type_str_to_enum(download->valuestring);
		}
		update = cJSON_GetObjectItemCaseSensitive(deployment, "update");
		if (cJSON_IsString(update) && update->valuestring != NULL) {
			deployment_data->update = update_type_str_to_enum(update->valuestring);
		}
		chunks = cJSON_GetObjectItemCaseSensitive(deployment, "chunks");
		if (cJSON_IsArray(chunks)) {
			cJSON_ArrayForEach(module, chunks) {
				if (cJSON_IsObject(module)) {
					identifier = cJSON_GetObjectItemCaseSensitive(module, "part");
					version = cJSON_GetObjectItemCaseSensitive(module, "version");
					artifacts = cJSON_GetObjectItemCaseSensitive(module, "artifacts");
					if (cJSON_IsString(identifier) && identifier->valuestring != NULL && 
						cJSON_IsString(version) && version->valuestring != NULL && 
						cJSON_IsArray(artifacts)) {
						module_type = module_type_str_to_enum(identifier->valuestring);
						if (module_type == HAWKBIT_CLIENT_MODULE_TYPE_ANKER) {
							strncpy(deployment_data->anker.version, version->valuestring, sizeof(deployment_data->anker.version));
							cJSON_ArrayForEach(artifact, artifacts) {
								json_parse_artifact(artifact, &deployment_data->anker);
							}
						}
					}
				}
			}
		}
	}

	cJSON_Delete(json);
	return HAWKBITCLIENT_OK;
}


uint8_t http_set_headers(esp_http_client_handle_t handle, const char *host, bool type) {
	int32_t ret;
	const char *h_host = "Host";
	const char *h_accept = "Accept";
	const char *h_accept_val = "application/hal+json";
	const char *h_content_type = "Content-Type";
	const char *h_content_type_val = "application/json;charset=UTF-8";
	const char *h_authorization_type = "Authorization";
	char *h_authorization_val;

	ret = esp_http_client_set_header(handle, h_host, host);
	if (ret != ESP_OK) {
		LOGE(TAG, "Failed to set host headers");
		return HAWKBITCLIENT_ERR_HEADERS;
	}
	LOGD(TAG, "Set http header [%s: %s]", h_host, host);

	if (type) {
		ret = esp_http_client_set_header(handle, h_accept, h_accept_val);
		if (ret != ESP_OK) {
			LOGE(TAG, "Failed to set accept headers");
			return HAWKBITCLIENT_ERR_HEADERS;
		}
		LOGD(TAG, "Set http header [%s: %s]", h_accept, h_accept_val);

		ret = esp_http_client_set_header(handle, h_content_type, h_content_type_val);
		if (ret != ESP_OK) {
			LOGE(TAG, "Failed to set content headers");
			return HAWKBITCLIENT_ERR_HEADERS;
		}
		LOGD(TAG, "Set http header [%s: %s]", h_content_type, h_content_type_val);
	}
	ret = asprintf(&h_authorization_val, "TargetToken %s", api_get_hb_security_token()->token);
	if (ret < 0) {
		LOGE(TAG, "Failed to allocate memory for authorization token");
		return HAWKBITCLIENT_ERR_NOMEMLEFT;
	}

	ret = esp_http_client_set_header(handle, h_authorization_type, h_authorization_val);
	if (ret != ESP_OK) {
		LOGE(TAG, "Failed to set authorization headers");
		FREE_MEM(h_authorization_val);
		return HAWKBITCLIENT_ERR_HEADERS;
	}
	LOGD(TAG, "Set http header [%s: %s]", h_authorization_type, h_authorization_val);
	FREE_MEM(h_authorization_val);

	return HAWKBITCLIENT_OK;
}


uint8_t http_request_type_get(const char *url, uint8_t **data, uint32_t *len) {
	int32_t ret;
	uint8_t err;
	int32_t status;
	int32_t contentLength;

	esp_http_client_config_t http_client_config = {
			.url = url,
			//.cert_pem = (char *)hawkbit_server_cert_pem,
			.method = HTTP_METHOD_GET,
			.timeout_ms = HAWKBIT_GET_TIMEOUT,
			//.user_agent = CONFIG_HTTP_CLIENT_USER_AGENT
	};

	esp_http_client_handle_t http_client_handle = esp_http_client_init(&http_client_config);
	if (!http_client_handle) {
		LOGE(TAG, "Failed to create http client handle");
		return HAWKBITCLIENT_ERR_HTTPCLIENT;
	}

	ret = http_set_headers(http_client_handle, UPDATE_ENDPOINT_URL, false);
	if (ret != HAWKBITCLIENT_OK) {
		LOGE(TAG, "Failed to set headers");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
		goto cleanup;
	}

	ret = esp_http_client_open(http_client_handle, 0);
	if (ret != ESP_OK) {
		LOGE(TAG, "Failed to open HTTP connection");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
		goto cleanup;
	}

	ret = esp_http_client_fetch_headers(http_client_handle);
	if (ret == ESP_FAIL) {
		LOGE(TAG, "Failed to fetch headers");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
		goto cleanup;
	}

	status = esp_http_client_get_status_code(http_client_handle);
	contentLength = esp_http_client_get_content_length(http_client_handle);

	if (status != 200) {
		LOGE(TAG, "Received wrong status code: %d", status);
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
		goto cleanup;
	}

	if (contentLength < 1 || contentLength > 4096) {
		LOGE(TAG, "Error, received invalid content length");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
		goto cleanup;
	}

	*data = (uint8_t *)malloc(contentLength);
	if (*data == NULL) {
		LOGE(TAG, "Failed to allocate memory for response data");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
		goto cleanup;
	}

	ret = esp_http_client_read_response(http_client_handle, (char *)*data, contentLength);
	if (ret > 0) {
		LOGD(TAG, "Received response with size: %d", ret);
		err = HAWKBITCLIENT_OK;
		*len = ret;
	}
	else {
		LOGE(TAG, "Failed to receive data from server");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
	}

cleanup:
	ret = esp_http_client_close(http_client_handle);
	if (ret != ESP_OK) {
		LOGE(TAG, "Failed to close client connection");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
	}

	ret = esp_http_client_cleanup(http_client_handle);
	if (ret != ESP_OK) {
		LOGE(TAG, "Failed to cleanup http client handle");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
	}

	if (err != HAWKBITCLIENT_OK) {
		LOGE(TAG, "Failed to handle request");
		FREE_MEM(*data);
		return HAWKBITCLIENT_ERR_HTTPCLIENT;
	}

	return HAWKBITCLIENT_OK;
}


uint8_t http_request_download_data(const char *url, const hawkbit_client_module_type_t module, const char *filename) {
	int32_t ret;
	int32_t size;
	uint8_t err;
	int32_t status;
	int32_t contentLength;
	uint16_t chunkSize;
	uint8_t *buffer = NULL;
	const esp_partition_t *updatePartition = NULL;
	esp_ota_handle_t updateHandle;

	esp_http_client_config_t http_client_config = {
			.url = url,
			.method = HTTP_METHOD_GET,
			.timeout_ms = HAWKBIT_DOWNLOAD_TIMEOUT,
			.keep_alive_enable = true
	};

	esp_http_client_handle_t http_client_handle = esp_http_client_init(&http_client_config);
	if (!http_client_handle) {
		LOGE(TAG, "Failed to create http client handle");
		return HAWKBITCLIENT_ERR_HTTPCLIENT;
	}

	ret = http_set_headers(http_client_handle, UPDATE_ENDPOINT_URL, false);
	if (ret != HAWKBITCLIENT_OK) {
		LOGE(TAG, "Failed to set headers");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
		goto cleanup;
	}

	ret = esp_http_client_open(http_client_handle, 0);
	if (ret != ESP_OK) {
		LOGE(TAG, "Failed to open HTTP connection");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
		goto cleanup;
	}

	ret = esp_http_client_fetch_headers(http_client_handle);
	if (ret == ESP_FAIL) {
		LOGE(TAG, "Failed to fetch headers");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
		goto cleanup;
	}

	status = esp_http_client_get_status_code(http_client_handle);
	contentLength = esp_http_client_get_content_length(http_client_handle);

	if (status != 200) {
		LOGE(TAG, "Received wrong status code: %d", status);
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
		goto cleanup;
	}

	if (contentLength < 1) {
		LOGE(TAG, "Error, received invalid content length");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
		goto cleanup;
	}

	if (module == HAWKBIT_CLIENT_MODULE_TYPE_ANKER) {
		chunkSize = HAWKBIT_DOWNLOAD_MODULE_CHUNK_SIZE;
	}
	else {
		chunkSize = HAWKBIT_DOWNLOAD_CONTROLLER_CHUNK_SIZE;
	}

	buffer = (uint8_t *)malloc(chunkSize);
	if (buffer == NULL) {
		LOGE(TAG, "Failed to allocate memory for response data");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
		goto cleanup;
	}

	if (module == HAWKBIT_CLIENT_MODULE_TYPE_ANKER) {
		updatePartition = esp_ota_get_next_update_partition(NULL);
		if (updatePartition == NULL) {
			LOGE(TAG, "Failed to get next partition for anker update");
			err = HAWKBITCLIENT_ERR_ESPOTA;
			goto cleanup;
		}
		LOGD(TAG, "Set next anker ota partition to address: 0x%08X", updatePartition->address);

		ret = esp_ota_begin(updatePartition, OTA_SIZE_UNKNOWN, &updateHandle);
		if (ret != ESP_OK) {
			LOGE(TAG, "Failed to begin anker update");
			err = HAWKBITCLIENT_ERR_ESPOTA;
			goto cleanup;
		}
	}

	while (true) {
		size = esp_http_client_read(http_client_handle, (char *)buffer, chunkSize);
		if (size < 0) {
			LOGE(TAG, "Failed to received data from server");
			err = HAWKBITCLIENT_ERR_HTTPCLIENT;
			break;
		}
		else if (size == 0) {
			LOGD(TAG, "Finished receiving data");
			err = HAWKBITCLIENT_OK;
			break;
		}

		LOGD(TAG, "Received chunk with size: %d", size);
		if (module == HAWKBIT_CLIENT_MODULE_TYPE_ANKER) {
			LOGD(TAG, "Write chunk with size: %d for anker to ota partition", size);
			ret = esp_ota_write(updateHandle, (const void *)buffer, size);
			if (ret != ESP_OK) {
				LOGE(TAG, "Failed to write to anker update partition");
				err = HAWKBITCLIENT_ERR_ESPOTA;
				break;
			}
		}
		/*else if (module == HAWKBIT_CLIENT_MODULE_TYPE_CELLULAR_MODULE) {
			LOGD(TAG, "Write chunk with size: %ld for cellular module to cellular component", size);
			if (cellular_write_update_file((char *)buffer, size) != CELLULAR_OK) {
                LOGE(TAG, "Failed to write to cellular module update file");
				err = HAWKBITCLIENT_ERR_CELLULAR;
				goto cleanup;
            }
		}
		else {
			LOGD(TAG, "Write chunk with size: %ld for controller to file: %s", size, filename);
			err = datastorage_write_chunked_binary_data_to_file_in_partition(filename, 0xFFFFFFFF, buffer, (uint32_t *)&size, false);
			if (err != DATASTORAGE_OK) {
				LOGE(TAG, "Failed to write data for controller to file: %s", filename);
				err = HAWKBITCLIENT_ERR_GENERAL;
				break;
			}
		}*/
	}

cleanup:
	if (module == HAWKBIT_CLIENT_MODULE_TYPE_ANKER) {
		ret = esp_ota_end(updateHandle);
		if (ret != ESP_OK) {
			LOGE(TAG, "Failed to end anker update");
			err = HAWKBITCLIENT_ERR_ESPOTA;
		}
		else {
			ret = esp_ota_set_boot_partition(updatePartition);
			if (ret != ESP_OK) {
				LOGE(TAG, "Failed to set anker boot partition");
				err = HAWKBITCLIENT_ERR_ESPOTA;
			}
		}
	}
	/*else if (module == HAWKBIT_CLIENT_MODULE_TYPE_CELLULAR_MODULE) {
		if (cellular_stop_update_download_process() != CELLULAR_OK) {
			LOGE(TAG, "Failed to stop cellular module update download process");
			err = HAWKBITCLIENT_ERR_CELLULAR;
		}
		cellular_set_back_into_normal_mode();
	}*/

	ret = esp_http_client_close(http_client_handle);
	if (ret != ESP_OK) {
		LOGE(TAG, "Failed to close client connection");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
	}

	ret = esp_http_client_cleanup(http_client_handle);
	if (ret != ESP_OK) {
		LOGE(TAG, "Failed to cleanup http client handle");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
	}

	FREE_MEM(buffer);
	if (err != HAWKBITCLIENT_OK) {
		LOGE(TAG, "Failed to handle request");
		return HAWKBITCLIENT_ERR_HTTPCLIENT;
	}

	return HAWKBITCLIENT_OK;
}


uint8_t http_request_type_post_put(esp_http_client_method_t method, uint32_t timeout, const char *url, uint8_t *data, uint32_t *len) {
	int32_t ret;
	uint8_t err;

	esp_http_client_config_t http_client_config = {
			.url = url,
			.method = method,
			.timeout_ms = (int)timeout
	};

	esp_http_client_handle_t http_client_handle = esp_http_client_init(&http_client_config);
	if (!http_client_handle) {
		LOGE(TAG, "Failed to create http client handle");
		return HAWKBITCLIENT_ERR_HTTPCLIENT;
	}

	ret = http_set_headers(http_client_handle, UPDATE_ENDPOINT_URL, true);
	if (ret != HAWKBITCLIENT_OK) {
		LOGE(TAG, "Failed to set headers");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
		goto cleanup;
	}

	ret = esp_http_client_open(http_client_handle, *len);
	if (ret != ESP_OK) {
		LOGE(TAG, "Failed to open HTTP connection");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
		goto cleanup;
	}

	ret = esp_http_client_write(http_client_handle, (const char *)data, *len);
	if (ret == ESP_FAIL) {
		LOGE(TAG, "Failed to send data to server");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
		goto cleanup;
	}

	ret = esp_http_client_fetch_headers(http_client_handle);
	if (ret == ESP_FAIL) {
		LOGE(TAG, "Failed to fetch headers");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
		goto cleanup;
	}

	ret = esp_http_client_get_status_code(http_client_handle);
	if (ret != 200) {
		LOGE(TAG, "Received wrong status code: %d", ret);
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
		goto cleanup;
	}
	LOGD(TAG, "Successfully send data to server");
	err = HAWKBITCLIENT_OK;

cleanup:
	ret = esp_http_client_close(http_client_handle);
	if (ret != ESP_OK) {
		LOGE(TAG, "Failed to close client connection");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
	}

	ret = esp_http_client_cleanup(http_client_handle);
	if (ret != ESP_OK) {
		LOGE(TAG, "Failed to cleanup http client handle");
		err = HAWKBITCLIENT_ERR_HTTPCLIENT;
	}

	if (err != HAWKBITCLIENT_OK) {
		LOGE(TAG, "Failed to handle request");
		return HAWKBITCLIENT_ERR_HTTPCLIENT;
	}

	return HAWKBITCLIENT_OK;
}


uint8_t retrieve_actions_to_do(hawkbit_client_base_content_t *content) {
	int32_t ret;
	char *url;
	uint8_t *buffer = NULL;
	uint32_t len;

	ret = asprintf(&url, "http://%s/%s/controller/v1/%s", UPDATE_ENDPOINT_URL, HAWKBIT_TENANT, api_get_serial_number()->serial);
	if (ret < 0) {
		LOGE(TAG, "Failed to allocate memory for request url");
		return HAWKBITCLIENT_ERR_NOMEMLEFT;
	}

	ret = http_request_type_get(url, &buffer, &len);
	FREE_MEM(url);
	if (ret != HAWKBITCLIENT_OK) {
		LOGE(TAG, "Failed to request actions to do from server");
		return HAWKBITCLIENT_ERR_HTTPCLIENT;
	}

	ret = json_parse_actions_to_do(buffer, content);
	FREE_MEM(buffer);
	if (ret != HAWKBITCLIENT_OK) {
		LOGE(TAG, "Failed to parse json to struct");
		return HAWKBITCLIENT_ERR_JSONPARSING;
	}

	LOGD(TAG, "Successfully requested new action states");

	return HAWKBITCLIENT_OK;
}


uint8_t retrieve_assigned_deployment(const char *deployment_url, hawkbit_client_deployment_t *deployment) {
	uint8_t ret;
	uint8_t *buffer = NULL;
	uint32_t len;

	ret = http_request_type_get(deployment_url, &buffer, &len);
	if (ret != HAWKBITCLIENT_OK) {
		LOGE(TAG, "Failed to request assigned deployment from server");
		return HAWKBITCLIENT_ERR_HTTPCLIENT;
	}

	ret = json_parse_deployment(buffer, deployment);
	FREE_MEM(buffer);
	if (ret != HAWKBITCLIENT_OK) {
		LOGE(TAG, "Failed to parse json to struct");
		return HAWKBITCLIENT_ERR_JSONPARSING;
	}

	LOGD(TAG, "Successfully requested assigned deployments");

	return HAWKBITCLIENT_OK;
}


uint8_t download_and_check_update_file(const char *url, const hawkbit_client_module_type_t module, const char *filename, const char *hash, uint8_t retries) {
	uint8_t ret;

	LOGD(TAG, "Downloading module: %d", module);
	do {
		ret = http_request_download_data(url, module, filename);
		if (ret == HAWKBITCLIENT_OK) {
			LOGD(TAG, "Successfully downloaded module: %d", module);
			return HAWKBITCLIENT_OK;
		}
		else {
			LOGW(TAG, "Failed to download module: %d, retries left: %d", module, retries);
		}
		vTaskDelay(pdMS_TO_TICKS(5000));
	} while (retries--);

	LOGE(TAG, "Failed to download module: %d, retries exceeded", module);
	return HAWKBITCLIENT_ERR_RETRIES_EXCEEDED;
}


uint8_t retrieve_update_artifacts(hawkbit_client_deployment_t *deployment) {
	uint8_t ret;
	timestamp_status_t timestamp_status;
	current_update_info_t update_info;
	uint32_t len_of_data = sizeof(current_update_info_t);
	update_info.update_id = deployment->id;

	if (deployment->anker.download_url[0] != '\0') {
		if (memcmp(&deployment->anker.download_url[strlen(deployment->anker.download_url) - sizeof(FIRMWARE_NAME_ANKER) + 1], FIRMWARE_NAME_ANKER, sizeof(FIRMWARE_NAME_ANKER)) == 0 && 
			deployment->anker.artifact_size <= FIRMWARE_MAX_SIZE_ANKER) {
            if (strcmp(deployment->anker.version, api_get_version()->version) == 0) {
                LOGD(TAG, "Anker version to install: %s is same as currently running one: %s, skip module", deployment->anker.version, api_get_version()->version);
                update_info.anker.status = CONTROLLER_UPDATE_STATUS_ALREADY_INSTALLED;
            }
            else {
                LOGD(TAG, "Anker version to install: %s is not same as currently running one: %s, download module", deployment->anker.version, api_get_version()->version);
                update_info.anker.status = CONTROLLER_UPDATE_STATUS_UPDATE_AVAILABLE;
            }

			if (update_info.anker.status == CONTROLLER_UPDATE_STATUS_UPDATE_AVAILABLE) {
				ret = download_and_check_update_file(deployment->anker.download_url, HAWKBIT_CLIENT_MODULE_TYPE_ANKER, FIRMWARE_NAME_ANKER, deployment->anker.hash_sha1, DOWNLOAD_RETRIES_ANKER);
				if (ret == HAWKBITCLIENT_OK) {
					LOGD(TAG, "Successfully download / installed data for anker");
					update_info.anker.status = CONTROLLER_UPDATE_STATUS_DOWNLOAD_SUCCESSFUL;
				}
				else {
					LOGE(TAG, "Failed to download / install data for anker");
					update_info.anker.status = CONTROLLER_UPDATE_STATUS_DOWNLOAD_FAILED;
				}
			}
		}
		else {
			LOGE(TAG, "Download data for anker are invalid");
			update_info.anker.status = CONTROLLER_UPDATE_STATUS_DOWNLOAD_INVALID;
		}

		if (api_get_timestamp_without_timezone(&timestamp_status, false) == API_OK) {
			LOGD(TAG, "Successfully requested current timestamp");
			update_info.anker.time_info.time_valid = true;
			update_info.anker.time_info.time_value = timestamp_status.timestamp;
		}
		else {
			LOGW(TAG, "Failed to get current unix timestamp");
			update_info.anker.time_info.time_valid = false;
			update_info.anker.time_info.time_value = esp_timer_get_time() / 1000000;
		}
	}
	else {
		update_info.anker.status = CONTROLLER_UPDATE_STATUS_UNKNOWN;
		update_info.anker.time_info.time_valid = false;
		update_info.anker.time_info.time_value = 0;
	}


	ret = datastorage_write_data_to_file_in_partition(UPDATER_FIRMWARE_FILENAME_UPDATE_STATUS, DATASTORAGE_WRITE_BINARY_DATA, (uint8_t *)&update_info, &len_of_data);
	if (ret != DATASTORAGE_OK) {
		LOGE(TAG, "Failed to write update status file");
		return HAWKBITCLIENT_ERR_GENERAL;
	}

	if (update_info.anker.status != CONTROLLER_UPDATE_STATUS_UNKNOWN && update_info.anker.status != CONTROLLER_UPDATE_STATUS_DOWNLOAD_SUCCESSFUL && update_info.anker.status != CONTROLLER_UPDATE_STATUS_ALREADY_INSTALLED) {
		LOGE(TAG, "Failed to download and check updates");
		return HAWKBITCLIENT_ERR_DOWNLOADING;
	}

	if (update_info.anker.status != CONTROLLER_UPDATE_STATUS_UNKNOWN && update_info.anker.status != CONTROLLER_UPDATE_STATUS_DOWNLOAD_SUCCESSFUL) {
		deployment->anker.download_url[0] = '\0';
	}

	LOGD(TAG, "Successfully downloaded and checked updates");
	return HAWKBITCLIENT_OK;
}


uint8_t send_update_feedback(hawkbit_deployment_feedback_t *feedback) {
	int32_t ret;
	timestamp_status_t timestamp_status;
	char *feedback_url;
	char *feedback_data;
	uint32_t feedback_data_len;

	ret = asprintf(&feedback_url, "http://%s/%s/controller/v1/%s/deploymentBase/%d/feedback", UPDATE_ENDPOINT_URL, HAWKBIT_TENANT, api_get_serial_number()->serial, feedback->id);
	if (ret < 0) {
		LOGE(TAG, "Failed to allocate memory for feedback url");
		return HAWKBITCLIENT_ERR_NOMEMLEFT;
	}

	if (api_get_timestamp_without_timezone(&timestamp_status, false) != API_OK) {
		LOGW(TAG, "Failed to get current unix timestamp");
		timestamp_status.timestamp = esp_timer_get_time() / 1000000;
	}

	ret = asprintf(&feedback_data, "{\"id\":\"%u\",\"time\":\"%lu\",\"status\":{\"result\":{\"finished\":\"%s\"},\"execution\":\"%s\",\"details\":[%s]}}",
									feedback->id, timestamp_status.timestamp, finish_type_to_str(feedback->finish_type), execution_type_to_str(feedback->execution_type), feedback->feedback_message_details);
	if (ret < 0) {
		LOGE(TAG, "Failed to allocate memory for feedback data");
		FREE_MEM(feedback_url);
		return HAWKBITCLIENT_ERR_NOMEMLEFT;
	}
	feedback_data_len = ret;
	LOGD(TAG, "Response msg: %s", feedback_data);

	ret = http_request_type_post_put(HTTP_METHOD_POST, HAWKBIT_POST_TIMEOUT, feedback_url, (uint8_t *)feedback_data, &feedback_data_len);
	FREE_MEM(feedback_url);
	FREE_MEM(feedback_data);
	if (ret != HAWKBITCLIENT_OK) {
		LOGE(TAG, "Failed to send feedback to server");
		return HAWKBITCLIENT_ERR_HTTPCLIENT;
	}

	return HAWKBITCLIENT_OK;
}


uint8_t check_installed_updates() {
	int32_t ret;
	bool file_status;
	const esp_partition_t *boot_partition;
	const esp_partition_t *running_partition;
	esp_ota_img_states_t running_image_state;
	uint8_t anker_update_value;
	current_update_info_t update_info;
	current_update_info_t *update_info_ptr = &update_info;
	uint32_t len_of_data = sizeof(current_update_info_t);
	hawkbit_deployment_feedback_t feedback_msg;
	timestamp_status_t timestamp_status;
	char time_string[24];
	char *tmp_string;

	boot_partition = esp_ota_get_boot_partition();
	running_partition = esp_ota_get_running_partition();
	LOGD(TAG, "Last anker boot partition at address: 0x%08X, running partition at adress: 0x%08X", boot_partition->address, running_partition->address);

	ret = esp_ota_get_state_partition(running_partition, &running_image_state);
	if (ret == ESP_ERR_NOT_SUPPORTED) {
		LOGD(TAG, "Currently running partition is no ota partition");
		running_image_state = ESP_OTA_IMG_UNDEFINED;
	}
	else if (ret != ESP_OK) {
		LOGE(TAG, "Failed to get information about running anker partition");
		return HAWKBITCLIENT_ERR_GENERAL;
	}
	else {
		LOGD(TAG, "Running anker partition currently is in state: %u", running_image_state);
	}

	if (running_image_state == ESP_OTA_IMG_PENDING_VERIFY) {
		ret = esp_ota_mark_app_valid_cancel_rollback();
		if (ret == ESP_OK) {
			LOGD(TAG, "Successfully set running partition as new boot partition");
			anker_update_value = 1;
		}
		else {
			LOGE(TAG, "Failed to set running partition as new boot partition");
			anker_update_value = 0;
		}
	}
	else if (boot_partition->address != running_partition->address) {
		LOGW(TAG, "Anker boot partition is not same as running partition, boot image is invalid");
		anker_update_value = 0;
	}
	else {
		LOGD(TAG, "Anker boot partition is same as running partition, boot image is valid");
		anker_update_value = 2;
	}

	ret = datastorage_check_file_existence(UPDATER_FIRMWARE_FILENAME_UPDATE_STATUS, &file_status);
	if (ret != DATASTORAGE_OK) {
		LOGE(TAG, "Failed to check update status file existence");
		return HAWKBITCLIENT_ERR_GENERAL;
	}
	if (!file_status) {
		LOGD(TAG, "No updates downloaded, skip status check");
		return HAWKBITCLIENT_OK;
	}

	ret = datastorage_read_data_from_file_in_partition(UPDATER_FIRMWARE_FILENAME_UPDATE_STATUS, DATASTORAGE_READ_BINARY_DATA, (uint8_t **)&update_info_ptr, &len_of_data);
	if (ret != DATASTORAGE_OK) {
		LOGE(TAG, "Failed to read update status file from partition");
		return HAWKBITCLIENT_ERR_GENERAL;
	}
	if (len_of_data != sizeof(current_update_info_t)) {
		LOGE(TAG, "Failed to read update status file, size invalid");
		return HAWKBITCLIENT_ERR_GENERAL;
	}
	if (update_info.anker.status == CONTROLLER_UPDATE_STATUS_DOWNLOAD_SUCCESSFUL) {
		if (anker_update_value == 2) {
			update_info.anker.status = CONTROLLER_UPDATE_STATUS_ALREADY_INSTALLED;
		}
		else if (anker_update_value == 1) {
			update_info.anker.status = CONTROLLER_UPDATE_STATUS_INSTALL_SUCCESSFUL;
		}
		else {
			update_info.anker.status = CONTROLLER_UPDATE_STATUS_INSTALL_FAILED;
		}
	}
    if (update_info.anker.status == CONTROLLER_UPDATE_STATUS_INSTALL_SUCCESSFUL || CONTROLLER_UPDATE_STATUS_ALREADY_INSTALLED) {
        update_info.anker.status = CONTROLLER_UPDATE_STATUS_HANDLE_P_ANKER_UPDATE;
	    datastorage_write_data_to_file_in_partition(UPDATER_FIRMWARE_FILENAME_UPDATE_STATUS, DATASTORAGE_WRITE_BINARY_DATA, (uint8_t *)&update_info, &len_of_data);
    }
	LOGD(TAG, "C-Anker: status: %u, time valid: %u - value: %u", update_info.anker.status, update_info.anker.time_info.time_valid, update_info.anker.time_info.time_value);
    if (update_info.anker.status == CONTROLLER_UPDATE_STATUS_HANDLE_P_ANKER_UPDATE) {
        feedback_msg.id = update_info.update_id;
        feedback_msg.finish_type = HAWKBIT_CLIENT_FINISH_TYPE_NONE;
        feedback_msg.execution_type = HAWKBIT_CLIENT_EXECUTION_TYPE_PROCEEDING;
        feedback_msg.feedback_message_details = NULL;
        if (update_info.anker.time_info.time_valid) {
            strftime(time_string, sizeof(time_string), "%d.%m.%Y %H:%M:%S UTC", gmtime((time_t *)&update_info.anker.time_info.time_value));
        }
        else {
            sprintf(time_string, "%us", update_info.anker.time_info.time_value);
        }
        asprintf(&feedback_msg.feedback_message_details, "\"C_ANKER: %s at %s\"", update_status_to_str(update_info.anker.status), time_string);
        if (feedback_msg.feedback_message_details == NULL) {
            asprintf(&feedback_msg.feedback_message_details, "\"Assigned update distribution not valid\"");
        }
        ret = send_update_feedback(&feedback_msg);
        FREE_MEM(feedback_msg.feedback_message_details);
        
        uint8_t cnt_to_update = 0;
        uint8_t cnt_joined = 0;
        uint8_t cnt_updated = 0;
	    taskRunState = HAWKBIT_TASK_RUN_STATE_WAIT;
        LOGI(TAG, "Handling p_anker updates");
        handle_p_anker_updates(&cnt_to_update, &cnt_joined, &cnt_updated);
        LOGI(TAG, "Found %u p_ankers to update, %u joined, %u updated", cnt_to_update, cnt_joined, cnt_updated);
        if (cnt_to_update > 0 && cnt_updated == 0) {
            update_info.anker.status = CONTROLLER_UPDATE_STATUS_INSTALL_FAILED;
        }
        else {
            update_info.anker.status = CONTROLLER_UPDATE_STATUS_HANDLED_P_ANKER_UPDATE;
        }
        update_info.anker.cnt_to_update = cnt_to_update;
        update_info.anker.cnt_joined = cnt_joined;
        update_info.anker.cnt_updated = cnt_updated;
	    datastorage_write_data_to_file_in_partition(UPDATER_FIRMWARE_FILENAME_UPDATE_STATUS, DATASTORAGE_WRITE_BINARY_DATA, (uint8_t *)&update_info, &len_of_data);
	    taskRunState = HAWKBIT_TASK_RUN_STATE_ACTIVE;
    }
	feedback_msg.id = update_info.update_id;
	if (update_info.anker.status == CONTROLLER_UPDATE_STATUS_HANDLED_P_ANKER_UPDATE || update_info.anker.status == CONTROLLER_UPDATE_STATUS_UNKNOWN || update_info.anker.status == CONTROLLER_UPDATE_STATUS_INSTALL_SUCCESSFUL || update_info.anker.status == CONTROLLER_UPDATE_STATUS_ALREADY_INSTALLED) {
		feedback_msg.finish_type = HAWKBIT_CLIENT_FINISH_TYPE_SUCCESS;
		feedback_msg.execution_type = HAWKBIT_CLIENT_EXECUTION_TYPE_CLOSED;
	}
	else {
		feedback_msg.finish_type = HAWKBIT_CLIENT_FINISH_TYPE_FAILURE;
		feedback_msg.execution_type = HAWKBIT_CLIENT_EXECUTION_TYPE_CLOSED;
	}

	feedback_msg.feedback_message_details = NULL;
	if (update_info.anker.status != CONTROLLER_UPDATE_STATUS_UNKNOWN) {
		if (update_info.anker.time_info.time_valid) {
			strftime(time_string, sizeof(time_string), "%d.%m.%Y %H:%M:%S UTC", gmtime((time_t *)&update_info.anker.time_info.time_value));
		}
		else {
			sprintf(time_string, "%us", update_info.anker.time_info.time_value);
		}

		asprintf(&feedback_msg.feedback_message_details, "\"C_ANKER: %s at %s (to_update: %hhu, joined: %hhu, updated: %hhu)\"", update_status_to_str(update_info.anker.status), time_string, update_info.anker.cnt_to_update, update_info.anker.cnt_joined, update_info.anker.cnt_updated);
	}
    if (feedback_msg.feedback_message_details == NULL) {
        asprintf(&feedback_msg.feedback_message_details, "\"Assigned update distribution not valid\"");
    }

	ret = send_update_feedback(&feedback_msg);
	FREE_MEM(feedback_msg.feedback_message_details);
	if (ret != HAWKBITCLIENT_OK) {
		LOGE(TAG, "Failed to send feedback");
		return HAWKBITCLIENT_ERR_HTTPCLIENT;
	}

	ret = datastorage_erase_file_from_partition(UPDATER_FIRMWARE_FILENAME_UPDATE_STATUS);
	if (ret != DATASTORAGE_OK) {
		LOGE(TAG, "Failed to delete update status file");
		return HAWKBITCLIENT_ERR_GENERAL;
	}

	LOGD(TAG, "Successfully finished update status check");
	return HAWKBITCLIENT_OK;
}


uint8_t update_polling_timeout(hawkbit_client_base_content_t *content, uint32_t *timeout) {
    if (content->time_until_next_poll < TIME_MS_UNTIL_NEXT_POLL_MIN || content->time_until_next_poll > TIME_MS_UNTIL_NEXT_POLL_MAX) {
        *timeout = TIME_MS_UNTIL_NEXT_POLL_DEFAULT_WIFI;
        LOGD(TAG, "Set hawkbit polling timeout to default: %ds, because we are connected to wifi and time is invalid", *timeout / 1000);
    }
    else {
        *timeout = content->time_until_next_poll;
        LOGD(TAG, "Set hawkbit polling timeout to: %ds, because we are connected to wifi", *timeout / 1000);
    }

	return HAWKBITCLIENT_OK;
}


uint8_t send_status_message() {
	int32_t ret;
	timestamp_status_t timestamp_status;
	char *status_url;
	char *status_data;
	uint32_t status_data_len;

	ret = asprintf(&status_url, "http://%s/%s/controller/v1/%s/configData", UPDATE_ENDPOINT_URL, HAWKBIT_TENANT, api_get_serial_number()->serial);
	if (ret < 0) {
		LOGE(TAG, "Failed to allocate memory for status message url");
		return HAWKBITCLIENT_ERR_NOMEMLEFT;
	}
	if (api_get_timestamp_without_timezone(&timestamp_status, false) != API_OK) {
		LOGW(TAG, "Failed to get current unix timestamp");
		timestamp_status.timestamp = esp_timer_get_time() / 1000000;
	}
	ret = asprintf(&status_data, "{\"data\":{\"TS\":\"%lu\",\"C_ANKER\":\"%s\"}}", timestamp_status.timestamp, api_get_version()->version);
	if (ret < 0) {
		LOGE(TAG, "Failed to allocate memory for status message data");
		FREE_MEM(status_url);
		return HAWKBITCLIENT_ERR_NOMEMLEFT;
	}
	status_data_len = ret;
	LOGD(TAG, "Response msg: %s", status_data);

	ret = http_request_type_post_put(HTTP_METHOD_PUT, HAWKBIT_PUT_TIMEOUT, status_url, (uint8_t *)status_data, &status_data_len);
	FREE_MEM(status_url);
	FREE_MEM(status_data);
	if (ret != HAWKBITCLIENT_OK) {
		LOGE(TAG, "Failed to send status message to server");
		return HAWKBITCLIENT_ERR_HTTPCLIENT;
	}

	return HAWKBITCLIENT_OK;
}


void hawkbit_task(void *pvParameters) {
	uint8_t ret;
	uint8_t err;
	EventBits_t uxBits;
	hawkbit_client_state_t clientState;
	uint32_t idleTimeout;
	uint32_t pollingTimeout;
	hawkbit_client_base_content_t baseContent;
	hawkbit_client_deployment_t deployment;

	LOGI(TAG, "Hawkbit task started");
	clientState = HAWKBIT_CLIENT_STATE_CHECK;
	idleTimeout = TIME_MS_UNTIL_NEXT_POLL_DEFAULT;
	pollingTimeout = TIME_MS_UNTIL_NEXT_POLL_DEFAULT;

	LOGI(TAG, "Hawkbit task starting handling");
	while (taskRunState != HAWKBIT_TASK_RUN_STATE_INACTIVE) {
		switch (clientState) {
			case HAWKBIT_CLIENT_STATE_IDLE: {
				LOGI(TAG, "Hawkbit state: IDLE");

				taskHandleRunning = false;
				uxBits = xEventGroupWaitBits(taskStatusBits, BIT_STOP | BIT_CP_STANDBY | BIT_REPOLL, pdTRUE, pdFALSE, idleTimeout / portTICK_PERIOD_MS);
				if (uxBits & BIT_STOP) {
					LOGD(TAG, "Stop bit was set, enter stop state");
					clientState = HAWKBIT_CLIENT_STATE_STOP;
					break;
				}
				else if (uxBits & BIT_CP_STANDBY) {
					LOGD(TAG, "CP standby got triggered, enter install state");
					clientState = HAWKBIT_CLIENT_STATE_INSTALL;
				}
				else if (taskRunState == HAWKBIT_TASK_RUN_STATE_BLOCKED) {
					LOGD(TAG, "Retrying update install, as last try failed");
					clientState = HAWKBIT_CLIENT_STATE_INSTALL;
				}
				else if (taskRunState == HAWKBIT_TASK_RUN_STATE_WAIT) {
					LOGD(TAG, "Keep update system in wait state until next power cycle, as mcu update install failed");
					clientState = HAWKBIT_CLIENT_STATE_IDLE;
				}
				else {
					if (statusMessageSend) {
						LOGD(TAG, "Timeout exceeded, enter polling state");
						clientState = HAWKBIT_CLIENT_STATE_POLL;
					}
					else {
						LOGD(TAG, "Timeout exceeded, status message not send, enter inform state");
						clientState = HAWKBIT_CLIENT_STATE_INFORM;
					}
				}
				taskHandleRunning = true;
				break;
			}
			case HAWKBIT_CLIENT_STATE_POLL: {
				LOGI(TAG, "Hawkbit state: POLL");

				ret = retrieve_actions_to_do(&baseContent);
				if (ret != HAWKBITCLIENT_OK) {
					LOGE(TAG, "Failed to handle actions request");
					goto poll_end;
				}
				ret = update_polling_timeout(&baseContent, &pollingTimeout);
				if (ret != HAWKBITCLIENT_OK) {
					LOGE(TAG, "Failed to set new polling timeout");
					goto poll_end;
				}
				idleTimeout = pollingTimeout;

				if (baseContent.deployment_url[0] != '\0') {
					LOGI(TAG, "Update(s) available for download");
					clientState = HAWKBIT_CLIENT_STATE_DOWNLOAD;
					break;
				}
				LOGI(TAG, "No updates for device available");

			poll_end:
				clientState = HAWKBIT_CLIENT_STATE_IDLE;
				break;
			}
			case HAWKBIT_CLIENT_STATE_DOWNLOAD: {
				LOGI(TAG, "Hawkbit state: DOWNLOAD");

				ret = retrieve_assigned_deployment((const char *)&baseContent.deployment_url, &deployment);
				if (ret != HAWKBITCLIENT_OK) {
					LOGE(TAG, "Failed to retrieve assigned deployment");
					goto download_end;
				}

				if (deployment.update != HAWKBIT_CLIENT_UPDATE_TYPE_FORCED && 
					deployment.update != HAWKBIT_CLIENT_UPDATE_TYPE_SOFT) {
					LOGW(TAG, "No valid update type requested, ignore update");
					goto download_end;
				}

				LOGI(TAG, "Retrieved url(s) for update download");

				ret = retrieve_update_artifacts(&deployment);
				if (ret == HAWKBITCLIENT_OK) {
					LOGI(TAG, "Successfully downloaded assigned artifacts");
					clientState = HAWKBIT_CLIENT_STATE_INSTALL;
					break;
				}
				else {
					LOGE(TAG, "Failed to retrieve update artifacts");
					if (ret == HAWKBITCLIENT_ERR_DOWNLOADING) {
						clientState = HAWKBIT_CLIENT_STATE_CHECK;
						break;
					}
				}

			download_end:
				clientState = HAWKBIT_CLIENT_STATE_IDLE;
				break;
			}
			case HAWKBIT_CLIENT_STATE_INSTALL: {
				LOGI(TAG, "Hawkbit state: INSTALL");

				if (deployment.anker.download_url[0] != '\0') {
					LOGI(TAG, "Found new anker software, restarting to complete install process");
					api_restart_anker();
				}

				clientState = HAWKBIT_CLIENT_STATE_CHECK;
				break;
			}
			case HAWKBIT_CLIENT_STATE_CHECK: {
				LOGI(TAG, "Hawkbit state: CHECK");

				if (check_installed_updates() != HAWKBITCLIENT_OK) {
					LOGE(TAG, "Failed to handle update state check");
				}
				else {
					LOGI(TAG, "Successfully checked install updates");
				}

				clientState = HAWKBIT_CLIENT_STATE_INFORM;
				break;
			}
			case HAWKBIT_CLIENT_STATE_INFORM: {
				LOGI(TAG, "Hawkbit state: INFORM");

				if (send_status_message() == HAWKBITCLIENT_OK) {
					LOGI(TAG, "Successfully send status message");
					statusMessageSend = true;
				}
				else {
					LOGE(TAG, "Failed to handle inform request sending");
					statusMessageSend = false;
				}

				clientState = HAWKBIT_CLIENT_STATE_POLL;
				break;
			}
			case HAWKBIT_CLIENT_STATE_STOP: {
				LOGI(TAG, "Hawkbit task stopped handling");
				taskRunState = HAWKBIT_TASK_RUN_STATE_INACTIVE;
				break;
			}
			default: {
				clientState = HAWKBIT_CLIENT_STATE_IDLE;
				break;
			}
		}
	}

	xEventGroupSetBits(taskStatusBits, BIT_STOPPED);
	vTaskDelete(NULL);
}


bool hawkbit_client_handle_is_running() {
	LOGD(TAG, "Hawkbit handle is currently running: %u", taskHandleRunning);

	return taskHandleRunning;
}


uint8_t hawkbit_client_trigger_event() {
	if (taskRunState == HAWKBIT_TASK_RUN_STATE_BLOCKED) {
		xEventGroupSetBits(taskStatusBits, BIT_CP_STANDBY);
		LOGD(TAG, "Set bit cp standby to notify hawkbit to install updates");
	}
	else {
		LOGD(TAG, "Hawkbit task is currently not in blocked state, ignore cp standby");
	}

	return HAWKBITCLIENT_OK;
}


uint8_t hawkbit_client_start() {
	if (taskRunState != HAWKBIT_TASK_RUN_STATE_INACTIVE) {
		statusMessageSend = false;
		xEventGroupSetBits(taskStatusBits, BIT_REPOLL);
		LOGD(TAG, "Hawkbit task is already started, set bit repoll to renew status");
		return HAWKBITCLIENT_OK;
	}

	LOGD(TAG, "Start hawkbit task");
	if (xTaskCreatePinnedToCore(hawkbit_task, "hawkbit_task", 8120, NULL, 5, NULL, PRO_CPU_NUM) != pdTRUE) {
		LOGE(TAG, "Failed to start hawkbit task");
		return HAWKBITCLIENT_ERR_GENERAL;
	}
	taskRunState = HAWKBIT_TASK_RUN_STATE_ACTIVE;
	LOGD(TAG, "Hawkbit task successfully started");

	return HAWKBITCLIENT_OK;
}


uint8_t hawkbit_client_stop() {
	if (taskRunState == HAWKBIT_TASK_RUN_STATE_INACTIVE) {
		LOGD(TAG, "Hawkbit task is currently not running");
		return HAWKBITCLIENT_OK;
	}
	else if (taskRunState == HAWKBIT_TASK_RUN_STATE_BLOCKED) {
		LOGW(TAG, "Could not stop hawkbit, task is currently in blocked state");
		return HAWKBITCLIENT_OK;
	}
	else if (taskRunState == HAWKBIT_TASK_RUN_STATE_WAIT) {
		LOGW(TAG, "Could not stop hawkbit, task is currently in wait state");
		return HAWKBITCLIENT_OK;
	}

	LOGD(TAG, "Stopping hawkbit task");
	xEventGroupSetBits(taskStatusBits, BIT_STOP);
	xEventGroupWaitBits(taskStatusBits, BIT_STOPPED, false, true, portMAX_DELAY);
	LOGD(TAG, "Hawkbit task successfully stopped");

	return HAWKBITCLIENT_OK;
}


uint8_t hawkbit_client_init() {
	taskStatusBits = xEventGroupCreate();

	return HAWKBITCLIENT_OK;
}
