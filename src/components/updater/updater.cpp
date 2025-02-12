#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <lwip/sockets.h>
#include <esp_ota_ops.h>
#include "esp_flash_partitions.h"
#include "freertos/projdefs.h"
#include "mbedtls/sha256.h" 
#include "logging.h"
#include "api.h"
#include "system.h"
#include "hawkbit/hawkbit_client.h"
#include "updater.h"
#include "../datastorage/datastorage.h"
#include "../wifi/wifi.h"
#include "types.h"


#define BROADCAST_IP ("255.255.255.255")
#define BROADCAST_PORT (8888)
#define ACK_PORT (8889)

#define P_ANKER_TCP_PORT (5000)

#define MAGIC_BYTES "ANK" // starting 3 bytes of each message
#define HASH_SIZE 32  // SHA256 hash size (32 bytes)
#define DATA_MAX_LEN (1400)
#define P_ANKER_UPDATE_STATUS "update.stat"

#define MAX_PARTITION_READ_FAILS (3)

#define MAX_SEND_FIRMWARE_BROADCAST_FAILS (10)
#define MAX_RETRIEVE_FIRMWARE_ALL_ACKS_FAILS (5)
#define MAX_RETRIEVE_FIRMWARE_ACKS_FAILS_TO_KICK_ANKER (5)
#define P_ANKER_CONNECT_TO_C_ANKER_AP_MAX_RETRIES (10)
#define P_ANKER_CONNECT_DEFAULT_AP_MAX_RETRIES (10)
#define MAX_AP_CREATE_RETRIES (10)
#define MAX_TCP_PING_RETRIES (3)
#define MAX_RETRIES_ACKS_UPDATED_P_ANKERS (29)
#define MAX_AP_JOINED_ACK_GET_RETRIES (10)
#define SET_P_ANKER_INTO_AP_MODE_MAX_RETRIES (10)

#define SEND_QUANTITY_ACKS_UPDATED_P_ANKERS (3)
#define SEND_SET_INTO_AP_MODE_QUANTITY (1)
#define SEND_AP_JOINED_ACK_QUANTITY (3)
#define SEND_FIRMWARE_DATA_QUANTITY (1)

#define TIME_WAIT_FOR_P_ANKER_TASK_STOP_MS (10000)

#define CHECK_MAGIC_BYTE(data) \
    ((data[0] == MAGIC_BYTES[0] && data[1] == MAGIC_BYTES[1] && data[2] == MAGIC_BYTES[2]))


static const char *TAG = "updater";


typedef enum {
    ACK_TYPE_HASH_ERROR = 0,
    ACK_TYPE_SEQ_ERROR = 1,
    ACK_TYPE_GENERAL_ERROR = 2,
    ACK_TYPE_KICK_FROM_UPDATE = 3,
    ACK_TYPE_VERSION = 4,
    ACK_TYPE_RCVD_BINARY_DATA = 5,
    ACK_TYPE_AP_JOIN = 6,
    ACK_TYPE_SUCCESS_UPDATE = 7,
    ACK_TYPE_FAIL_UPDATE = 8,
} ack_type_t;


typedef enum {
    UPDATE_PING_TYPE_GET_VERSION = 0,
    UPDATE_PING_TYPE_CONNECT_TO_AP = 1,
    UPDATE_PING_TYPE_SEND_FIRMWARE_DATA = 2,
    UPDATE_PING_TYPE_GET_UPDATE_VERSION_ACK = 3,
} update_ping_type_t;


typedef enum {
    COMMUNNICATE_P_ANKER_TYPE_SET_INTO_AP_MODE = 0,
    COMMUNNICATE_P_ANKER_TYPE_ACK_CONNECT = 1,
    COMMUNNICATE_P_ANKER_TYPE_SEND_UPDATE_FILE = 2,
    COMMUNNICATE_P_ANKER_TYPE_ACK_UPDATE = 3,
} communicate_p_anker_type_t;


typedef struct update_info {
    uint8_t magic[3];
    uint8_t type;
    uint8_t serial[SERIAL_SIZE];
    uint8_t hash[HASH_SIZE];
    uint16_t seq;
    uint8_t is_last;
    uint16_t data_len;
    uint8_t data[DATA_MAX_LEN];
} update_ping_info_t;


typedef struct {
    uint8_t magic[3];
    uint8_t type;
    uint8_t serial[SERIAL_SIZE];
    uint8_t hash[HASH_SIZE];
    uint16_t seq;
    uint8_t version_len;
    char version[MAX_VERSION_SIZE];
} update_ping_ack_t;


typedef struct {
    char serial[SERIAL_SIZE];
    uint32_t ip;
    bool acknowledged;
    uint8_t fail_cnt;
} p_ankers_to_update_t;


typedef enum {
    UPDATE_STATUS_DOWNLOADING = 0,
    UPDATE_STATUS_INSTALLED = 1,
} p_anker_update_status_t;


typedef struct {
    p_anker_update_status_t status;
    char serial[SERIAL_SIZE];
} p_anker_update_info_t;


const int UDP_TASK_STOPPED = BIT0;
const int TCP_TASK_STOPPED = BIT1;
bool udp_task_running = false;
bool tcp_task_running = false;
EventGroupHandle_t p_anker_task_event_group;


static void calculate_update_ping_info_hash(const update_ping_info_t *update_info, uint8_t *hash) {
    mbedtls_sha256_context sha256;
    mbedtls_sha256_init(&sha256);
    mbedtls_sha256_starts(&sha256, 0);
    mbedtls_sha256_update(&sha256, &update_info->type, sizeof(update_info->type));
    mbedtls_sha256_update(&sha256, (uint8_t *)update_info->serial, SERIAL_SIZE);
    mbedtls_sha256_update(&sha256, (uint8_t *)&update_info->seq, sizeof(update_info->seq));
    mbedtls_sha256_update(&sha256, (uint8_t *)&update_info->is_last, sizeof(update_info->is_last));
    mbedtls_sha256_update(&sha256, (uint8_t *)&update_info->data_len, sizeof(update_info->data_len));
    mbedtls_sha256_update(&sha256, update_info->data, update_info->data_len);
    mbedtls_sha256_finish(&sha256, hash);
    mbedtls_sha256_free(&sha256);
}


static void calculate_update_ping_ack_hash(const update_ping_ack_t *update_info, uint8_t *hash) {
    mbedtls_sha256_context sha256;
    mbedtls_sha256_init(&sha256);
    mbedtls_sha256_starts(&sha256, 0);
    mbedtls_sha256_update(&sha256, &update_info->type, 1);
    mbedtls_sha256_update(&sha256, update_info->serial, SERIAL_SIZE);
    mbedtls_sha256_update(&sha256, (uint8_t *)&update_info->seq, sizeof(update_info->seq));
    mbedtls_sha256_update(&sha256, &update_info->version_len, sizeof(update_info->version_len));
    mbedtls_sha256_update(&sha256, (uint8_t *)update_info->version, update_info->version_len);
    mbedtls_sha256_finish(&sha256, hash);
    mbedtls_sha256_free(&sha256);
}


static uint8_t send_update_ping_info_broadcast(int sock, struct sockaddr_in *addr, update_ping_type_t type, uint8_t quantity, uint16_t seq, bool is_last, uint8_t *data, uint16_t data_len) {
    uint8_t rc;
    serial_number_t sn;
    update_ping_info_t *update_info = (update_ping_info_t *)malloc(sizeof(update_ping_info_t));
    if (update_info == NULL) {
        LOGE(TAG, "Failed to allocate memory for update ping info");
        return UPDATER_ERROR;
    }
    memcpy(update_info->magic, MAGIC_BYTES, sizeof(update_info->magic));
    update_info->type = (uint8_t)type;
    memcpy(update_info->serial, api_get_serial_number()->serial, sizeof(update_info->serial));
    if (type == UPDATE_PING_TYPE_SEND_FIRMWARE_DATA) {
        update_info->seq = seq;
        update_info->data_len = data_len;
        update_info->is_last = is_last;
        memcpy(update_info->data, data, data_len);
    }
    else {
        update_info->seq = 0;
        update_info->data_len = 0;
        update_info->is_last = 0;
    }
    calculate_update_ping_info_hash(update_info, update_info->hash);
    for (uint8_t i = 0; i < quantity; i++) {
        if (sendto(sock, (void *)update_info, sizeof(update_ping_info_t), 0, (struct sockaddr *)addr, sizeof(*addr)) < 0) {
            LOGE(TAG, "Failed to send update ping info");
            rc = UPDATER_ERROR;
            goto exit;
        }
    }
    
    rc = UPDATER_OK;
    exit:
    FREE_MEM(update_info);
    return rc;
}


static uint8_t send_update_ping_info_direct(update_ping_type_t type, uint32_t ip) {
    uint8_t rc;
    uint8_t i;
    struct sockaddr_in addr;
    update_ping_info_t update_info;
    update_ping_ack_t ack_info;
    int sock = -1;
    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        LOGE(TAG, "Failed to create socket");
        rc = UPDATER_ERROR;
        goto exit;
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons(P_ANKER_TCP_PORT);
    addr.sin_addr.s_addr = ip;

    LOGD(TAG, "Attempting to connect to IP %s", inet_ntoa(addr.sin_addr));
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        LOGE(TAG, "Failed to set socket timeout");
        rc = UPDATER_ERROR;
        goto exit;
    }
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE(TAG, "Failed to connect to %s", inet_ntoa(addr.sin_addr));
        rc = UPDATER_ERROR;
        goto exit;
    }

    memcpy(update_info.magic, MAGIC_BYTES, sizeof(update_info.magic));
    update_info.type = (uint8_t)type;
    memcpy(update_info.serial, api_get_serial_number()->serial, sizeof(update_info.serial));
    update_info.seq = 0;
    update_info.data_len = 0;
    calculate_update_ping_info_hash(&update_info, update_info.hash);

    for (i = 0; i < MAX_TCP_PING_RETRIES; i++) {
        if (send(sock, &update_info, sizeof(update_ping_info_t), 0) < 0) {
            LOGE(TAG, "Failed to send update ping info");
            rc = UPDATER_ERROR;
            goto exit;
        }
        ssize_t response_len = recv(sock, &ack_info, sizeof(update_ping_ack_t), 0);
        if (response_len > 0) {
            LOGD(TAG, "Received response from %s", inet_ntoa(addr.sin_addr));
            if (CHECK_MAGIC_BYTE(ack_info.magic)) {
                uint8_t hash[HASH_SIZE];
                calculate_update_ping_ack_hash(&ack_info, hash);
                if (memcmp(hash, ack_info.hash, HASH_SIZE) != 0) {
                    LOGE(TAG, "Received invalid ack hash. Retry...");
                    continue;
                }
                if (ack_info.type == ACK_TYPE_AP_JOIN) {
                    break;
                }
                LOGE(TAG, "Received invalid ack type: %u. Retry...", ack_info.type);
            }
        }
    }
    if (i == MAX_TCP_PING_RETRIES) {
        LOGE(TAG, "Received no valid acks.");
        rc = UPDATER_ERROR;
        goto exit;
    }

    rc = UPDATER_OK;

exit:
    if (sock > 0) {
        close(sock);
    }
    return rc;
}


static void retrieve_update_ping_ack(int sock, struct sockaddr_in *addr, update_ping_type_t type, uint16_t seq, uint8_t *num_p_ankers, p_ankers_to_update_t **p_ankers, uint32_t timeout_ms, uint8_t max_cnt_p_ankers) {
    update_ping_ack_t ack_info;
    uint32_t start_time = esp_timer_get_time() / 1000;
    uint8_t i;

    *num_p_ankers = 0;
    while (esp_timer_get_time() / 1000 - start_time < timeout_ms) {
        struct sockaddr_in sender_addr;
        socklen_t sender_addr_len = sizeof(sender_addr);
        char response[256];
        ssize_t response_len = recvfrom(sock, response, sizeof(response), 0, (struct sockaddr *)&sender_addr, &sender_addr_len);
        if (response_len > 0) {
            LOGD(TAG, "Received response from %s", inet_ntoa(sender_addr.sin_addr));
            memcpy(&ack_info, response, sizeof(update_ping_ack_t));
            if (CHECK_MAGIC_BYTE(ack_info.magic)) {
                uint8_t hash[HASH_SIZE];
                calculate_update_ping_ack_hash(&ack_info, hash);
                if (memcmp(hash, ack_info.hash, HASH_SIZE) != 0) {
                    LOGE(TAG, "Received invalid ack hash");
                    continue;
                }
                for (i = 0; i < *num_p_ankers; i++) {
                    if (memcmp(p_ankers[i]->serial, ack_info.serial, SERIAL_SIZE) == 0) {
                        break;
                    }
                }
                if (i != *num_p_ankers) {
                    LOGD(TAG, "Serial %.*s already in use", SERIAL_SIZE, ack_info.serial);
                    continue;
                }
                if (ack_info.version_len < MAX_VERSION_SIZE) {
                    ack_info.version[ack_info.version_len] = '\0';
                }
                // here you can add more checks if needed (specifc version checks...)
                // --> this is the reason why the c_anker does not transmit the version and the p_anker does only respond if the version unmatches
                if (type == UPDATE_PING_TYPE_GET_VERSION || type == UPDATE_PING_TYPE_CONNECT_TO_AP) {
                    if (type == UPDATE_PING_TYPE_GET_VERSION && ack_info.type != ACK_TYPE_VERSION) {
                        LOGE(TAG, "Received invalid type");
                        continue;
                    }
                    else if (type == UPDATE_PING_TYPE_CONNECT_TO_AP && ack_info.type != ACK_TYPE_AP_JOIN) {
                        LOGE(TAG, "Received invalid type");
                        continue;
                    }
                    if (strncmp(ack_info.version, api_get_version()->version, sizeof(ack_info.version)) == 0) {
                        LOGD(TAG, "Serial %.*s is already on the latest version", SERIAL_SIZE, ack_info.serial);
                        continue;
                    }
                    p_ankers[*num_p_ankers] = (p_ankers_to_update_t *)malloc(sizeof(p_ankers_to_update_t));
                    memcpy(p_ankers[*num_p_ankers]->serial, ack_info.serial, SERIAL_SIZE);
                    p_ankers[*num_p_ankers]->ip = sender_addr.sin_addr.s_addr;
                    p_ankers[*num_p_ankers]->acknowledged = false;
                    *num_p_ankers += 1;
                    if (*num_p_ankers == max_cnt_p_ankers) {
                        return;
                    }
                }
                else if (type == UPDATE_PING_TYPE_GET_UPDATE_VERSION_ACK) {
                    if (ack_info.type != ACK_TYPE_SUCCESS_UPDATE && ack_info.type != ACK_TYPE_FAIL_UPDATE) {
                        LOGE(TAG, "Received invalid type");
                        continue;
                    }
                    p_ankers[*num_p_ankers] = (p_ankers_to_update_t *)malloc(sizeof(p_ankers_to_update_t));
                    memcpy(p_ankers[*num_p_ankers]->serial, ack_info.serial, SERIAL_SIZE);
                    p_ankers[*num_p_ankers]->ip = sender_addr.sin_addr.s_addr;
                    LOGD(TAG, "Received ack %u from anker %.*s with version %s", ack_info.type, SERIAL_SIZE, p_ankers[*num_p_ankers]->serial, ack_info.version);
                    if (ack_info.type == ACK_TYPE_SUCCESS_UPDATE && strncmp(ack_info.version, api_get_version()->version, sizeof(ack_info.version)) == 0) {
                        p_ankers[*num_p_ankers]->acknowledged = true;
                    }
                    else {
                        p_ankers[*num_p_ankers]->acknowledged = false;
                    }
                    *num_p_ankers += 1;
                    if (*num_p_ankers == max_cnt_p_ankers) {
                        return;
                    }
                }
                else {
                    if (ack_info.type != ACK_TYPE_RCVD_BINARY_DATA && ack_info.type != ACK_TYPE_KICK_FROM_UPDATE && ack_info.type != ACK_TYPE_GENERAL_ERROR) {
                        LOGE(TAG, "Received invalid type from %.*s stop every response must be acknowledged", SERIAL_SIZE, ack_info.serial);
                        return;
                    }
                    if (ack_info.type == ACK_TYPE_RCVD_BINARY_DATA) {
                        if (ack_info.seq < seq) {
                            LOGI(TAG, "Received old resend of old seq");
                            continue;
                        }
                        else if (ack_info.seq > seq) {
                            LOGE(TAG, "Received newer seq than sent");
                            continue;
                        }
                    }
                    p_ankers[*num_p_ankers] = (p_ankers_to_update_t *)malloc(sizeof(p_ankers_to_update_t));
                    memcpy(p_ankers[*num_p_ankers]->serial, ack_info.serial, SERIAL_SIZE);
                    p_ankers[*num_p_ankers]->ip = sender_addr.sin_addr.s_addr;
                    if (ack_info.type == ACK_TYPE_RCVD_BINARY_DATA) {
                        if (ack_info.seq < seq) {
                            LOGW(TAG, "Received resend of seq");
                        }
                        LOGD(TAG, "Received correct ack from anker %.*s", SERIAL_SIZE, ack_info.serial);
                        p_ankers[*num_p_ankers]->acknowledged = true;
                        p_ankers[*num_p_ankers]->fail_cnt = 0;
                    }
                    else if (ack_info.type == ACK_TYPE_GENERAL_ERROR) {
                        LOGD(TAG, "Received general error from anker %.*s", SERIAL_SIZE, ack_info.serial);
                        p_ankers[*num_p_ankers]->acknowledged = true;
                        p_ankers[*num_p_ankers]->fail_cnt = 1;
                    }
                    else {
                        LOGD(TAG, "Received kick request from anker %.*s", SERIAL_SIZE, ack_info.serial);
                        p_ankers[*num_p_ankers]->acknowledged = false;
                        p_ankers[*num_p_ankers]->fail_cnt = 0;
                    }
                    *num_p_ankers += 1;
                    if (*num_p_ankers == max_cnt_p_ankers) {
                        return;
                    }
                }
            }
            else {
                LOGD(TAG, "Received invalid magic bytes");
            }
        }
        else {
            LOGD(TAG, "No response received");
        }
    }
}


static uint8_t communicate_with_p_ankers(communicate_p_anker_type_t type, p_ankers_to_update_t **p_ankers, uint8_t *num_p_ankers, uint8_t *num_acknowledged) {
    uint8_t rc;
    struct sockaddr_in broadcast_addr;
    struct sockaddr_in ack_addr;
    int broadcast_socket = -1;
    int ack_socket = -1;
    int broadcast_opt = 1;
    int ack_opt = 1;
    struct timeval tv = {1, 0};
    update_ping_info_t update_info;

    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(BROADCAST_PORT);
    broadcast_addr.sin_addr.s_addr = inet_addr(BROADCAST_IP);
    ack_addr.sin_family = AF_INET;
    ack_addr.sin_port = htons(ACK_PORT);
    ack_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    broadcast_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (broadcast_socket <= 0) {
        LOGE(TAG, "Failed to create broadcast socket");
        rc = UPDATER_ERROR;
        goto exit;
    }
    if (setsockopt(broadcast_socket, SOL_SOCKET, SO_REUSEADDR, &broadcast_opt, sizeof(broadcast_opt)) < 0) {
        LOGE(TAG, "Failed to set broadcast ack socket reuse");
        rc = UPDATER_ERROR;
        goto exit;
    }
    if (setsockopt(broadcast_socket, SOL_SOCKET, SO_BROADCAST, &broadcast_opt, sizeof(broadcast_opt)) < 0) {
        LOGE(TAG, "Failed to set broadcast option");
        rc = UPDATER_ERROR;
        goto exit;
    }
    ack_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ack_socket <= 0) {
        LOGE(TAG, "Failed to create ack socket");
        rc = UPDATER_ERROR;
        goto exit;
    }
    if (setsockopt(ack_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        LOGE(TAG, "Failed to set ack socket timeout");
        rc = UPDATER_ERROR;
        goto exit;
    }
    if (setsockopt(ack_socket, SOL_SOCKET, SO_REUSEADDR, &ack_opt, sizeof(ack_opt)) < 0) {
        LOGE(TAG, "Failed to set ack socket reuse");
        rc = UPDATER_ERROR;
        goto exit;
    }
    if (bind(ack_socket, (struct sockaddr *)&ack_addr, sizeof(ack_addr)) < 0) {
        LOGE(TAG, "Failed to bind ack socket");
        rc = UPDATER_ERROR;
        goto exit;
    }
    if (type == COMMUNNICATE_P_ANKER_TYPE_SET_INTO_AP_MODE) {
        for (uint8_t i = 0; i < SET_P_ANKER_INTO_AP_MODE_MAX_RETRIES; i++) {
            send_update_ping_info_broadcast(broadcast_socket, &broadcast_addr, UPDATE_PING_TYPE_GET_VERSION, SEND_SET_INTO_AP_MODE_QUANTITY, 0, false, NULL, 0);
            retrieve_update_ping_ack(ack_socket, &ack_addr, UPDATE_PING_TYPE_GET_VERSION, 0, num_p_ankers, p_ankers, 1000, MAX_P_ANKER_TO_UPDATE);
            if (*num_p_ankers == MAX_P_ANKER_TO_UPDATE) {
                break;
            }
        }
        if (num_p_ankers == 0) {
            LOGI(TAG, "No p_ankers to update found");
            rc = UPDATER_OK;
            goto exit;
        }
        for (uint8_t i = 0; i < *num_p_ankers; i++) {
            LOGI(TAG, "Send AP connect start ping to p_anker with serial %.*s", SERIAL_SIZE, p_ankers[i]->serial);
            if (send_update_ping_info_direct(UPDATE_PING_TYPE_CONNECT_TO_AP, p_ankers[i]->ip) != UPDATER_OK) {
                LOGW(TAG, "Panker with serial %.*s is NOT ACK", SERIAL_SIZE, p_ankers[i]->serial);
                p_ankers[i]->acknowledged = false;
            }
            else {
                LOGI(TAG, "Panker with serial %.*s is ACK", SERIAL_SIZE, p_ankers[i]->serial);
                p_ankers[i]->acknowledged = true;
                *num_acknowledged += 1;
            }
        }
    }
    else if (type == COMMUNNICATE_P_ANKER_TYPE_ACK_CONNECT) {
        for (uint8_t i = 0; i < MAX_AP_JOINED_ACK_GET_RETRIES; i++) {
            send_update_ping_info_broadcast(broadcast_socket, &broadcast_addr, UPDATE_PING_TYPE_GET_VERSION, SEND_AP_JOINED_ACK_QUANTITY, 0,  false, NULL, 0);
            retrieve_update_ping_ack(ack_socket, &ack_addr, UPDATE_PING_TYPE_GET_VERSION, 0, num_p_ankers, p_ankers, 1000, *num_acknowledged);
            if (*num_p_ankers == *num_acknowledged) {
                break;
            }
        }
    }
    else if (type == COMMUNNICATE_P_ANKER_TYPE_SEND_UPDATE_FILE) {
        uint16_t seq = 0;
        uint8_t data[DATA_MAX_LEN];
        uint16_t data_len = 0;
        size_t offset = 0;
        bool is_last = false;
        p_ankers_to_update_t **ack_p_ankers = (p_ankers_to_update_t **)malloc(*num_p_ankers * sizeof(p_ankers_to_update_t *));
        uint8_t num_ack_p_ankers = 0;
        uint32_t start_time;
        uint8_t small_time_diff_cnt = 0;

        uint8_t read_partition_fail_cnt = 0;
        uint8_t send_fail_cnt = 0;
        uint8_t all_acks_fail_cnt = 0;
        uint8_t num_remaining_p_ankers = *num_p_ankers;
        uint8_t num_correct_answered;

        const esp_partition_t *running_partition = esp_ota_get_running_partition();
        if (running_partition == NULL) {
            LOGE(TAG, "Failed to get running partition");
            for (uint8_t i = 0; i < *num_p_ankers; i++) {
                FREE_MEM(ack_p_ankers[i]);
            }
            FREE_MEM(ack_p_ankers);
            rc = UPDATER_ERROR;
            goto exit;
        }
        LOGD(TAG, "Size of running partition: %d", running_partition->size);
        while(offset < running_partition->size) {
            data_len = sizeof(data);
            if (offset + data_len > running_partition->size) {
                data_len = running_partition->size - offset;
                is_last = true;
            }
            if (esp_partition_read(running_partition, offset, data, (size_t)data_len) != ESP_OK) {
                if (read_partition_fail_cnt < MAX_PARTITION_READ_FAILS) {
                    read_partition_fail_cnt++;
                    LOGE(TAG, "Failed to read partition. Retry");
                    continue;
                }
                else {
                    LOGE(TAG, "Failed to read partition 3 times. Stop update");
                    rc = UPDATER_ERROR;
                    goto exit;
                }
            }
            read_partition_fail_cnt = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
            if (send_update_ping_info_broadcast(broadcast_socket, &broadcast_addr, UPDATE_PING_TYPE_SEND_FIRMWARE_DATA, SEND_FIRMWARE_DATA_QUANTITY, seq, is_last, data, data_len) != UPDATER_OK) {
                if (send_fail_cnt < MAX_SEND_FIRMWARE_BROADCAST_FAILS) {
                    send_fail_cnt++;
                    LOGE(TAG, "Failed to send update binary. Retry");
                    continue;
                }
                else {
                    LOGE(TAG, "Failed to send update binary 3 times. Stop update");
                    rc = UPDATER_ERROR;
                    goto exit;
                }
            }
            send_fail_cnt = 0;
            retrieve_update_ping_ack(ack_socket, &ack_addr, UPDATE_PING_TYPE_SEND_FIRMWARE_DATA, seq, &num_ack_p_ankers, ack_p_ankers, seq == 0 ? 2000 : 1000, num_remaining_p_ankers);
            if (num_ack_p_ankers == 0) {
                if (all_acks_fail_cnt < MAX_RETRIEVE_FIRMWARE_ALL_ACKS_FAILS) {
                    all_acks_fail_cnt++;
                    LOGE(TAG, "No p_ankers answered correct. Retry");
                    continue;
                }
                else {
                    LOGE(TAG, "No p_ankers answered correct. Stop update");
                    rc = UPDATER_ERROR;
                    goto exit;
                }
            }
            all_acks_fail_cnt = 0;
            uint8_t j;
            num_correct_answered = 0;
            for (uint8_t i = 0; i < num_remaining_p_ankers; i++) {
                if (p_ankers[i]->fail_cnt >= MAX_RETRIEVE_FIRMWARE_ACKS_FAILS_TO_KICK_ANKER) {
                    continue;
                }
                for (j = 0; j < num_ack_p_ankers; j++) {
                    if (memcmp(ack_p_ankers[j]->serial, p_ankers[i]->serial, SERIAL_SIZE) == 0) {
                        if (ack_p_ankers[j]->fail_cnt) {
                            LOGW(TAG, "Anker %.*s answered with error", SERIAL_SIZE, p_ankers[i]->serial);
                            p_ankers[i]->fail_cnt++;
                            if (p_ankers[i]->fail_cnt < MAX_RETRIEVE_FIRMWARE_ACKS_FAILS_TO_KICK_ANKER) {
                                LOGW(TAG, "Anker %.*s failed. Retry cnt: %u", SERIAL_SIZE, p_ankers[i]->serial, p_ankers[i]->fail_cnt);
                            }
                            else {
                                LOGW(TAG, "Anker %.*s will be kicked due to his request", SERIAL_SIZE, p_ankers[i]->serial);
                                p_ankers[i]->fail_cnt = MAX_RETRIEVE_FIRMWARE_ACKS_FAILS_TO_KICK_ANKER;
                                if (num_remaining_p_ankers > 0) {
                                    num_remaining_p_ankers--;
                                }
                            }
                        }
                        else if (!ack_p_ankers[j]->acknowledged) {
                            LOGW(TAG, "Anker %.*s will be kicked due to his request", SERIAL_SIZE, p_ankers[i]->serial);
                            p_ankers[i]->fail_cnt = MAX_RETRIEVE_FIRMWARE_ACKS_FAILS_TO_KICK_ANKER;
                            num_remaining_p_ankers--;
                        }
                        else {
                            num_correct_answered++;
                            p_ankers[i]->fail_cnt = 0;
                        }
                        break;
                    }
                }
                if (j == num_ack_p_ankers) {
                    p_ankers[i]->fail_cnt++;
                    if (p_ankers[i]->fail_cnt < MAX_RETRIEVE_FIRMWARE_ACKS_FAILS_TO_KICK_ANKER) {
                        LOGW(TAG, "Anker %.*s failed. Retry cnt: %u", SERIAL_SIZE, p_ankers[i]->serial, p_ankers[i]->fail_cnt);
                    }
                    else {
                        LOGW(TAG, "Anker %.*s will be kicked due to his request", SERIAL_SIZE, p_ankers[i]->serial);
                        p_ankers[i]->fail_cnt = MAX_RETRIEVE_FIRMWARE_ACKS_FAILS_TO_KICK_ANKER;
                        num_remaining_p_ankers--;
                    }
                }
            }
            if (num_remaining_p_ankers == 0) {
                LOGI(TAG, "All ankers failed update!");
                rc = UPDATER_ERROR;
                goto exit;
            }
            if (num_correct_answered != num_remaining_p_ankers) {
                LOGE(TAG, "%u out of %u ankers answered correct. Retry", num_correct_answered, num_remaining_p_ankers);
                continue;
            }
            LOGI(TAG, "All remaining p_ankers answered correct");
            offset += data_len;
            seq++;
        }
        LOGI(TAG, "Wrote Binary file to all remaining Ankers");
        for (uint8_t i = 0; i < *num_p_ankers; i++) {
            FREE_MEM(ack_p_ankers[i]);
        }
        FREE_MEM(ack_p_ankers);
        *num_acknowledged = num_remaining_p_ankers;
    }
    else if (type == COMMUNNICATE_P_ANKER_TYPE_ACK_UPDATE) {
        p_ankers_to_update_t **ack_p_ankers = (p_ankers_to_update_t **)malloc(*num_p_ankers * sizeof(p_ankers_to_update_t *));
        if (ack_p_ankers == NULL) {
            LOGE(TAG, "Failed to allocate memory for ack_p_ankers");
        }
        uint8_t num_ack_p_ankers = 0;
        uint8_t num_answered = 0;
        uint8_t num_correct_answered = 0;
        uint8_t y;
        for (y = 0; y < MAX_RETRIES_ACKS_UPDATED_P_ANKERS; y++) {
            send_update_ping_info_broadcast(broadcast_socket, &broadcast_addr, UPDATE_PING_TYPE_GET_UPDATE_VERSION_ACK, SEND_QUANTITY_ACKS_UPDATED_P_ANKERS, 0,  false, NULL, 0);
            retrieve_update_ping_ack(ack_socket, &ack_addr, UPDATE_PING_TYPE_GET_UPDATE_VERSION_ACK, 0, &num_ack_p_ankers, ack_p_ankers, 10000, *num_p_ankers);
            uint8_t j;
            for (uint8_t i = 0; i < *num_p_ankers; i++) {
                if (p_ankers[i]->fail_cnt >= MAX_RETRIEVE_FIRMWARE_ACKS_FAILS_TO_KICK_ANKER) {
                    continue;
                }
                if (p_ankers[i]->acknowledged) {
                    continue;
                }
                for (j = 0; j < num_ack_p_ankers; j++) {
                    if (memcmp(ack_p_ankers[j]->serial, p_ankers[i]->serial, SERIAL_SIZE) == 0) {
                        p_ankers[i]->acknowledged = ack_p_ankers[j]->acknowledged;
                        if (p_ankers[i]->acknowledged) {
                            num_correct_answered++;
                        }
                        num_answered++;
                        break;
                    }
                }
            }
            if (num_answered == *num_acknowledged) {
                break;
            }
        }
        if (y == 20) {
            if (num_answered == 0) {
                LOGE(TAG, "No p_ankers answered");
                rc = UPDATER_ERROR;
                goto exit;
            }
        }
        LOGI(TAG, "%u out of %u answered and %u got updated", num_answered, *num_acknowledged, num_correct_answered);
        *num_acknowledged = num_correct_answered;
        for (uint8_t i = 0; i < *num_p_ankers; i++) {
            FREE_MEM(ack_p_ankers[i]);
        }
        FREE_MEM(ack_p_ankers);
    }

    rc = UPDATER_OK;
    exit:
    if (broadcast_socket > 0) {
        close(broadcast_socket);
    }
    if (ack_socket > 0) {
        close(ack_socket);
    }
    return rc;
}


static uint8_t send_ack(int sock, struct sockaddr_in *addr,  ack_type_t type, uint16_t seq) {
    uint8_t rc;
    update_ping_ack_t update_info;
    update_info.type = type;
    update_info.seq = seq;
    memcpy(update_info.magic, MAGIC_BYTES, sizeof(update_info.magic));
    memcpy(update_info.serial, api_get_serial_number()->serial, sizeof(update_info.serial));

    switch (type) {
        case ACK_TYPE_SUCCESS_UPDATE:
        case ACK_TYPE_VERSION:
            update_info.version_len = strlen(api_get_version()->version);
            strncpy(update_info.version, api_get_version()->version, update_info.version_len);
            calculate_update_ping_ack_hash(&update_info, update_info.hash);
            if (sendto(sock, &update_info, (sizeof(update_ping_ack_t) - sizeof(update_info.version)) + update_info.version_len, 0, (struct sockaddr *)addr, sizeof(struct sockaddr_in)) < 0) {
                LOGE(TAG, "Failed to send ACK type: %u", type);
                rc = UPDATER_ERROR;
                goto exit;
            }
            break;
        default:
        update_info.version_len = 0;
        calculate_update_ping_ack_hash(&update_info, update_info.hash);
        if (sendto(sock, &update_info, (sizeof(update_ping_ack_t) - sizeof(update_info.version)), 0, (struct sockaddr *)addr, sizeof(struct sockaddr_in)) < 0) {
            LOGE(TAG, "Failed to send ACK type: %u", type);
            rc = UPDATER_ERROR;
            goto exit;
        }
        break;
    }

    rc = UPDATER_OK;
    exit:
    return rc;
}


static uint8_t stop_p_anker_poll_mode() {
    bool wait_for_all_tasks;
    uint8_t ux_bits;

    if (udp_task_running || tcp_task_running) {
        xEventGroupClearBits(p_anker_task_event_group, UDP_TASK_STOPPED | TCP_TASK_STOPPED);
        if (udp_task_running && tcp_task_running) {
            wait_for_all_tasks = true;
        }
        else {
            wait_for_all_tasks = false;
        }
        udp_task_running = false;
        tcp_task_running = false;
        ux_bits = xEventGroupWaitBits(p_anker_task_event_group, UDP_TASK_STOPPED | TCP_TASK_STOPPED, pdTRUE, wait_for_all_tasks ? pdTRUE : pdFALSE, pdMS_TO_TICKS(TIME_WAIT_FOR_P_ANKER_TASK_STOP_MS));
        if (!ux_bits) {
            LOGE(TAG, "Failed to stop p-anker polling");
            return UPDATER_ERROR;
        }
    }
    LOGI(TAG, "P-anker polling stopped");

    return UPDATER_OK;
}


static void calculate_c_anker_ap_ssid_and_password(const char *serial, char *ssid, char *password) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < (SERIAL_SIZE - 1); j++) {
            if (i == 3 && j == SERIAL_SIZE - 2) {
                break;
            }
            char ssid_char;
            char password_char;

            ssid_char = ((serial[j] + i + j) % 58) + 65;  // Wrap within ASCII: A-Z, a-z, 0-9
            if (ssid_char >= 91 && ssid_char <= 96) {
                ssid_char += 6; // Skip characters like `[\]^_`
            }
            if (ssid_char >= 123) {
                ssid_char = 48 + (ssid_char - 123); // Wrap to 0-9 if goes above `z`
            }
            password_char = ((serial[j] + i + j + 40) % 90);  // Wrap within ASCII: 40-59, A-Z, a-z
            if (password_char >= 40 && password_char <= 59) {
                password_char += 0;  // Keep within special characters 40-59
            } else if (password_char >= 65 && password_char <= 90) {
                password_char += 0;  // Keep A-Z
            } else if (password_char >= 97 && password_char <= 122) {
                password_char += 0;  // Keep a-z
            } else {
                password_char = 40 + ((serial[j] + i + j) % 20); // Wrap within 40-59
            }
            ssid[j + (SERIAL_SIZE - 1) * i] = ssid_char;
            password[j + (SERIAL_SIZE - 1) * i] = password_char;
        }
    }
    ssid[AP_SSID_SIZE - 1] = '\0';
    password[AP_PASSWORD_SIZE - 1] = '\0';
}


static void p_anker_connect_to_c_anker_ap_task(void *c_anker_serial) {
    if (stop_p_anker_poll_mode() != UPDATER_OK) {
        LOGE(TAG, "Could not stop p-anker polling");
        FREE_MEM(c_anker_serial);
        vTaskDelete(NULL);
    }
    uint8_t i;
    for (i = 0; i < P_ANKER_CONNECT_TO_C_ANKER_AP_MAX_RETRIES; i++) {
        LOGI(TAG, "Waiting for connect to C-Anker AP");
        char ssid[AP_SSID_SIZE];
        char password[AP_PASSWORD_SIZE];
        calculate_c_anker_ap_ssid_and_password((const char *)c_anker_serial, ssid, password);
        if (wifi_connect_to_ap(ssid, password) == WIFI_OK) {
            break;
        }
        LOGW(TAG, "Failed to connect to C-Anker AP, retry...");
    }
    if (i == P_ANKER_CONNECT_TO_C_ANKER_AP_MAX_RETRIES) {
        LOGE(TAG, "Failed to connect to C-Anker AP");
    }
    FREE_MEM(c_anker_serial);
    vTaskDelete(NULL);
}


static void p_anker_connect_to_default_ap() {
    if (stop_p_anker_poll_mode() != UPDATER_OK) {
        LOGE(TAG, "Could not stop p-anker polling");
        vTaskDelete(NULL);
    }
    uint8_t i;
    for (i = 0; i < P_ANKER_CONNECT_DEFAULT_AP_MAX_RETRIES; i++) {
        LOGI(TAG, "Waiting for connect to default AP");
        if (wifi_connect_to_ap(NULL, NULL) == WIFI_OK) {
            break;
        }
        LOGW(TAG, "Failed to connect to default AP, retry...");
    }
    if (i == P_ANKER_CONNECT_DEFAULT_AP_MAX_RETRIES) {
        LOGE(TAG, "Failed to connect to default AP");
    }
    datastorage_erase_file_from_partition(P_ANKER_UPDATE_STATUS);
    vTaskDelete(NULL);
}


static void p_anker_udp_broadcast_task() {
    uint8_t rc;
    struct sockaddr_in broadcast_addr;
    struct sockaddr_in ack_addr;
    int broadcast_socket = -1;
    int ack_socket = -1;
    int broadcast_opt = 1;
    int ack_opt = 1;
    struct timeval tv = {5, 0};
    update_ping_info_t *update_info = (update_ping_info_t *)malloc(sizeof(update_ping_info_t));
    uint32_t start_time;
    uint8_t small_time_diff_cnt = 0;
    uint16_t seq = 0;
    uint16_t offset = 0;
    const esp_partition_t *update_partition = NULL;
	esp_ota_handle_t update_handle;
    p_anker_update_info_t update_status;
    uint32_t len_of_data = sizeof(p_anker_update_info_t);
    char *response = (char *)malloc(sizeof(update_ping_info_t) + 1);

    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(BROADCAST_PORT);
    broadcast_addr.sin_addr.s_addr = inet_addr(BROADCAST_IP);
    ack_addr.sin_family = AF_INET;
    ack_addr.sin_port = htons(ACK_PORT);
    ack_addr.sin_addr.s_addr = inet_addr(BROADCAST_IP);

    broadcast_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (broadcast_socket <= 0) {
        LOGE(TAG, "Failed to create broadcast socket");
        rc = UPDATER_ERROR;
        goto exit;
    }
    if (setsockopt(broadcast_socket, SOL_SOCKET, SO_REUSEADDR, &broadcast_opt, sizeof(broadcast_opt)) < 0) {
        LOGE(TAG, "Failed to set broadcast ack socket reuse");
        rc = UPDATER_ERROR;
        goto exit;
    }
    if (setsockopt(broadcast_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        LOGE(TAG, "Failed to set ack socket timeout");
        rc = UPDATER_ERROR;
        goto exit;
    }
    if (bind(broadcast_socket, (struct sockaddr *)&broadcast_addr, sizeof(ack_addr)) < 0) {
        LOGE(TAG, "Failed to bind ack socket");
        rc = UPDATER_ERROR;
        goto exit;
    }

    ack_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ack_socket <= 0) {
        LOGE(TAG, "Failed to create ack ksocket");
        rc = UPDATER_ERROR;
        goto exit;
    }
    if (setsockopt(ack_socket, SOL_SOCKET, SO_BROADCAST, &ack_opt, sizeof(ack_opt)) < 0) {
        LOGE(TAG, "Failed to set ack broadcastoption");
        rc = UPDATER_ERROR;
        goto exit;
    }
    if (setsockopt(ack_socket, SOL_SOCKET, SO_REUSEADDR, &ack_opt, sizeof(ack_opt)) < 0) {
        LOGE(TAG, "Failed to set ack socket reuse");
        rc = UPDATER_ERROR;
        goto exit;
    }

    udp_task_running = true;
    start_time = esp_timer_get_time() / 1000;
    while (udp_task_running) {
        struct sockaddr_in sender_addr;
        socklen_t sender_addr_len = sizeof(sender_addr);
        vTaskDelay(pdMS_TO_TICKS(10));
        ssize_t response_len = recvfrom(broadcast_socket, response, sizeof(update_ping_info_t), 0, (struct sockaddr *)&sender_addr, &sender_addr_len);

        if (response_len > 0) {
            start_time = esp_timer_get_time() / 1000;
            memcpy(update_info, response, sizeof(update_ping_info_t));
            if (!CHECK_MAGIC_BYTE(update_info->magic)) {
                LOGW(TAG, "Received invalid magic bytes");
                continue;
            }
            uint8_t hash[HASH_SIZE];
            calculate_update_ping_info_hash(update_info, hash);
            if (memcmp(update_info->hash, hash, HASH_SIZE) != 0) {
                LOGE(TAG, "Received invalid hash");
                send_ack(ack_socket, &ack_addr, ACK_TYPE_HASH_ERROR, 0);
            }
            else {
                LOGI(TAG, "Received valid hash");
                if (update_info->type == UPDATE_PING_TYPE_SEND_FIRMWARE_DATA) {
                    LOGD(TAG, "Received firmware data with seq: %u", update_info->seq);
                    if (seq != update_info->seq) {
                        if (update_info->seq < seq) {
                            LOGW(TAG, "Received resend of seq");
                            send_ack(ack_socket, &ack_addr,  ACK_TYPE_RCVD_BINARY_DATA, update_info->seq);
                            continue;
                        }
                        else {
                            LOGE(TAG, "Received invalid seq");
                            send_ack(ack_socket, &ack_addr, ACK_TYPE_SEQ_ERROR, seq);
                            continue;
                        }
                    }
                    if (seq == 0) {
                        update_partition = esp_ota_get_next_update_partition(NULL);
                        if (update_partition == NULL) {
                            LOGE(TAG, "Failed to get next partition for update");
                            send_ack(ack_socket, &ack_addr, ACK_TYPE_GENERAL_ERROR, seq);
                            continue;
                        }
                        LOGI(TAG, "Set next ota partition to address: 0x%08X", update_partition->address);
                        if (esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle)!= ESP_OK) {
                            LOGE(TAG, "Failed to begin update");
                            send_ack(ack_socket, &ack_addr, ACK_TYPE_GENERAL_ERROR, seq);
                            continue;
                        }
                    }
                    if (esp_ota_write(update_handle, (const void *)update_info->data, update_info->data_len) != ESP_OK) {
                        LOGE(TAG, "Failed to write to  update partition");
                        send_ack(ack_socket, &ack_addr, ACK_TYPE_GENERAL_ERROR, seq);
                        continue;
                    }
                    if (update_info->is_last) {
                        LOGI(TAG, "Wrote full binary data to ota partition");
                        if (esp_ota_end(update_handle) != ESP_OK) {
                            LOGE(TAG, "Failed to end update");
                            send_ack(ack_socket, &ack_addr, ACK_TYPE_GENERAL_ERROR, seq);
                            continue;
                        }
                        if (esp_ota_set_boot_partition(update_partition)) {
                            send_ack(ack_socket, &ack_addr, ACK_TYPE_GENERAL_ERROR, seq);
                            continue;
                        }
                        send_ack(ack_socket, &ack_addr,  ACK_TYPE_RCVD_BINARY_DATA, seq);
                        update_status.status = UPDATE_STATUS_INSTALLED;
                        memcpy(update_status.serial, update_info->serial, SERIAL_SIZE);
                        if (datastorage_write_data_to_file_in_partition(P_ANKER_UPDATE_STATUS, DATASTORAGE_WRITE_BINARY_DATA, (uint8_t *)&update_status, &len_of_data) != DATASTORAGE_OK) {
                            LOGE(TAG, "Failed to write update status file");
                        }
                        api_restart_anker();
                    }
                    send_ack(ack_socket, &ack_addr,  ACK_TYPE_RCVD_BINARY_DATA, seq);
                    seq++;
                }
                else if (update_info->type == UPDATE_PING_TYPE_GET_UPDATE_VERSION_ACK) {
                    const esp_partition_t *running_partition;
                    esp_ota_img_states_t running_image_state;

	                running_partition = esp_ota_get_running_partition();
                    if (running_partition == NULL) {
                        LOGE(TAG, "Failed to get running partition");
                        send_ack(ack_socket, &ack_addr, ACK_TYPE_FAIL_UPDATE, 0);
                        p_anker_connect_to_default_ap();
                        continue;
                    }
                    if (esp_ota_get_state_partition(running_partition, &running_image_state) != ESP_OK) {
                        LOGE(TAG, "Failed to get information about running smart module partition");
                        send_ack(ack_socket, &ack_addr,  ACK_TYPE_FAIL_UPDATE, 0);
                        p_anker_connect_to_default_ap();
                        continue;
                    }
                    LOGD(TAG, "Running smart module partition currently is in state: %u", running_image_state);
                    if (running_image_state == ESP_OTA_IMG_PENDING_VERIFY) {
                        if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
                            LOGE(TAG, "Failed to set running partition as new boot partition");
                            send_ack(ack_socket, &ack_addr,  ACK_TYPE_FAIL_UPDATE, 0);
                            p_anker_connect_to_default_ap();
                            continue;
                        }
                    }
                    else if (running_image_state == ESP_OTA_IMG_ABORTED || running_image_state == ESP_OTA_IMG_INVALID) {
                        LOGE(TAG, "Image has invalid state");
                        send_ack(ack_socket, &ack_addr,  ACK_TYPE_FAIL_UPDATE, 0);
                        p_anker_connect_to_default_ap();
                        continue;
                    }
                    send_ack(ack_socket, &ack_addr,  ACK_TYPE_SUCCESS_UPDATE ,0);
                    p_anker_connect_to_default_ap();
                }
                else {
                    send_ack(ack_socket, &ack_addr,  ACK_TYPE_VERSION, 0);
                }
            }
        }
        else {
            LOGD(TAG, "No response received");
        }
    }

    rc = UPDATER_OK;
    exit:
    if (broadcast_socket > 0) {
        close(broadcast_socket);
    }
    if (ack_socket > 0) {
        close(ack_socket);
    }
    FREE_MEM(update_info);
    FREE_MEM(response);
    xEventGroupSetBits(p_anker_task_event_group, UDP_TASK_STOPPED);
    vTaskDelete(NULL);
}



static uint8_t stop_p_anker_poll_mode_and_connect_to_c_anker_ap(char *serial) {
    char *c_anker_serial = (char *)malloc(SERIAL_SIZE);
    memcpy(c_anker_serial, serial, SERIAL_SIZE);

    xTaskCreatePinnedToCore((TaskFunction_t)p_anker_connect_to_c_anker_ap_task, "p_anker_connect_to_c_anker_task", 4096, c_anker_serial, 5, NULL, 0);
    return UPDATER_OK;
}



static void p_anker_tcp_task() {
    int server_sock = -1;
    int client_sock = -1;
    struct sockaddr_in server_addr;
    socklen_t server_addr_len = sizeof(server_addr);
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    update_ping_info_t update_info;
    struct timeval tv = {5, 0};
    int opt = 1;
    char *request = (char *)malloc(sizeof(update_ping_info_t));
    p_anker_update_info_t update_status;
	uint32_t len_of_data = sizeof(p_anker_update_info_t);

    if((server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        LOGE(TAG, "Failed to create server socket");
        goto exit;
    }
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(P_ANKER_TCP_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (setsockopt(server_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        LOGE(TAG, "Failed to set ack socket timeout");
        goto exit;
    }
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOGE(TAG, "Failed to set ack socket opt");
        goto exit;
    }
    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        LOGE(TAG, "Failed to bind server socket");
        goto exit;
    }
    if (listen(server_sock, 1) < 0) {
        LOGE(TAG, "Failed to listen on server socket");
        goto exit;
    }
    LOGI(TAG, "Listening on port %u", P_ANKER_TCP_PORT);
    tcp_task_running = true;
    while (tcp_task_running) {
        if ((client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_addr_len)) < 0) {
            LOGI(TAG, "Timeout in accept");
            continue;
        }
        LOGI(TAG, "Accepted connection from %s", inet_ntoa(client_addr.sin_addr));
        ssize_t response_len = recv(client_sock, request, sizeof(update_ping_info_t), 0);
        if (response_len > 0) {
            LOGD(TAG, "Received response from %s", inet_ntoa(client_addr.sin_addr));
            memcpy(&update_info, request, sizeof(update_ping_info_t));
            if (CHECK_MAGIC_BYTE(update_info.magic)) {
                uint8_t hash[HASH_SIZE];
                calculate_update_ping_info_hash(&update_info, hash);
                if (memcmp(update_info.hash, hash, HASH_SIZE) != 0) {
                    LOGE(TAG, "Received invalid hash");
                    send_ack(client_sock, &client_addr,  ACK_TYPE_HASH_ERROR, 0);
                }
                else {
                    if (update_info.type == UPDATE_PING_TYPE_CONNECT_TO_AP) {
                        send_ack(client_sock, &client_addr,  ACK_TYPE_AP_JOIN, 0);
                        update_status.status = UPDATE_STATUS_DOWNLOADING;
                        memcpy(update_status.serial, update_info.serial, SERIAL_SIZE);
                        if (datastorage_write_data_to_file_in_partition(P_ANKER_UPDATE_STATUS, DATASTORAGE_WRITE_BINARY_DATA, (uint8_t *)&update_status, &len_of_data) != DATASTORAGE_OK) {
                            LOGE(TAG, "Failed to write update status file");
                        }
                        stop_p_anker_poll_mode_and_connect_to_c_anker_ap(update_status.serial);

                    }
                    else {
                        LOGE(TAG, "Received invalid type");
                    }
                }
            }
        }
        close(client_sock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    exit:
    if (server_sock > 0) {
        close(server_sock);
    }
    if (client_sock > 0) {
        close(client_sock);
    }   
    FREE_MEM(request);
    xEventGroupSetBits(p_anker_task_event_group, TCP_TASK_STOPPED);
    vTaskDelete(NULL);
}


uint8_t handle_p_anker_updates(uint8_t *cnt_to_update, uint8_t *cnt_joined, uint8_t *cnt_updated) {
    uint8_t rc;
    p_ankers_to_update_t **p_ankers = (p_ankers_to_update_t **)malloc(MAX_P_ANKER_TO_UPDATE * sizeof(p_ankers_to_update_t *));
    uint8_t num_p_ankers = 0;
    uint8_t num_acknowledged = 0;
    bool created_ap = false;
    p_ankers_to_update_t **joined_p_ankers = NULL;
    uint8_t num_joined_p_ankers = 0;
    uint8_t num_ack_updated_p_ankers = 0;


    *cnt_to_update = 0;
    *cnt_joined = 0;
    *cnt_updated = 0;
    communicate_with_p_ankers(COMMUNNICATE_P_ANKER_TYPE_SET_INTO_AP_MODE, p_ankers, &num_p_ankers, &num_acknowledged);
    // Here you can add specific handling based on number of not ack'ed devices
    if (num_acknowledged == 0) {
        LOGE(TAG, "No p_ankers are acknowledged, stop update");
        rc = UPDATER_ERROR;
        goto exit;
    }
    else if (num_acknowledged == num_p_ankers) {
        LOGI(TAG, "All p_ankers are acknowledged");
    }
    else {
        LOGI(TAG, "%u out of %u p_ankers are acknowledged, continue update", num_acknowledged, num_p_ankers);
    }
    *cnt_to_update = num_p_ankers;
    uint8_t x;
    char ssid[AP_SSID_SIZE];
    char password[AP_PASSWORD_SIZE];
    calculate_c_anker_ap_ssid_and_password(api_get_serial_number()->serial, ssid, password);
    for (x = 0; x < MAX_AP_CREATE_RETRIES; x++) {
        if (wifi_create_ap(ssid, password) == WIFI_OK) {
            break;
        }
        LOGW(TAG, "Failed to create AP, retry...");
    }
    if (x == 10) {
        LOGE(TAG, "Failed to create AP");
        rc = UPDATER_ERROR;
        goto exit;
    }
    created_ap = true;
    joined_p_ankers = (p_ankers_to_update_t **)malloc(num_acknowledged * sizeof(p_ankers_to_update_t *));
    communicate_with_p_ankers(COMMUNNICATE_P_ANKER_TYPE_ACK_CONNECT, joined_p_ankers, &num_joined_p_ankers, &num_acknowledged);
    // Here you can add specific handling based on number of not joined devices
    if (num_acknowledged == num_joined_p_ankers) {
        LOGI(TAG, "All ack p_ankers joined AP");
    }
    else if (num_joined_p_ankers > 0) {
        LOGI(TAG, "%u out of %u ack p_ankers joined AP", num_joined_p_ankers, num_acknowledged);
    }
    else {
        LOGE(TAG, "No ack p_ankers joined AP");
        rc = UPDATER_ERROR;
        goto exit;
    }
    *cnt_joined = num_joined_p_ankers;
    for (uint8_t i = 0; i < num_joined_p_ankers; i++) {
        joined_p_ankers[i]->fail_cnt = 0;
    }
    if (communicate_with_p_ankers(COMMUNNICATE_P_ANKER_TYPE_SEND_UPDATE_FILE, joined_p_ankers, &num_joined_p_ankers, &num_acknowledged) != UPDATER_OK) {
        LOGE(TAG, "Failed to update p_ankers");
        rc = UPDATER_ERROR;
        goto exit;
    }
    if (num_acknowledged != num_joined_p_ankers) {
        LOGW(TAG, "%u out of %u ack p_ankers received full update file", num_acknowledged, num_joined_p_ankers);
    }
    else {
        LOGI(TAG, "All ack p_ankers received full update file");
    }
    num_ack_updated_p_ankers = num_acknowledged;
    if (communicate_with_p_ankers(COMMUNNICATE_P_ANKER_TYPE_ACK_UPDATE, joined_p_ankers, &num_joined_p_ankers, &num_ack_updated_p_ankers) != UPDATER_OK) {
        LOGE(TAG, "Failed to ack updated p_ankers");
        rc = UPDATER_ERROR;
        goto exit;
    }
    if (num_ack_updated_p_ankers != num_acknowledged) {
        LOGW(TAG, "%u out of %u p_ankers got updated", num_ack_updated_p_ankers, num_acknowledged);
    }
    else {
        LOGI(TAG, "All p_ankers got updated");
    }
    *cnt_updated = num_ack_updated_p_ankers;

    rc = UPDATER_OK;
    exit:
    for (uint8_t i = 0; i < num_p_ankers; i++) {
        FREE_MEM(p_ankers[i]);
    }
    FREE_MEM(p_ankers);
    for (uint8_t i = 0; i < num_joined_p_ankers; i++) {
        FREE_MEM(joined_p_ankers[i]);
    }
    FREE_MEM(joined_p_ankers);
    if (created_ap) {
        wifi_connect_to_ap((const char*)NULL, (const char*)NULL);
    }

    return rc;
}



static uint8_t set_p_anker_into_poll_mode() {
    if (!udp_task_running) {
        udp_task_running = true;
        xTaskCreatePinnedToCore((TaskFunction_t)p_anker_udp_broadcast_task, "p_anker_udp", 4096, NULL, 5, NULL, 0);
    }
    if (!tcp_task_running) {
        tcp_task_running = true;
        xTaskCreatePinnedToCore((TaskFunction_t)p_anker_tcp_task, "p_anker_tcp", 4096, NULL, 5, NULL, 0);
    }

    return UPDATER_OK;
}



uint8_t updater_stop(device_type_t type) {
    if (type == DEVICE_TYPE_C_ANKER) {
        if (hawkbit_client_stop() != HAWKBITCLIENT_OK) {
            LOGD(TAG, "Could not stop hawkbit client");
            return UPDATER_ERROR;
        }
    }
    else {
        if (stop_p_anker_poll_mode() != UPDATER_OK) {
            LOGD(TAG, "Could not stop p-anker polling");
            return UPDATER_ERROR;
        }
    }

    return UPDATER_OK;
}


uint8_t updater_start(device_type_t type) {
    if (type == DEVICE_TYPE_C_ANKER) {
        if (hawkbit_client_start() != HAWKBITCLIENT_OK) {
            LOGD(TAG, "Could not start hawkbit client");
            return UPDATER_ERROR;
        }
    }
    else {
        static bool initial_done = false;
        esp_err_t ret;
        bool file_status;
        const esp_partition_t *boot_partition;
        const esp_partition_t *running_partition;
        esp_ota_img_states_t running_image_state;
        p_anker_update_info_t update_info;
        p_anker_update_info_t *update_info_ptr = &update_info;
        uint32_t len_of_data = sizeof(p_anker_update_info_t);

        if (!initial_done) {
            boot_partition = esp_ota_get_boot_partition();
            running_partition = esp_ota_get_running_partition();
            LOGD(TAG, "Last smart module boot partition at address: 0x%08X, running partition at adress: 0x%08X", boot_partition->address, running_partition->address);

            
            if ((ret = esp_ota_get_state_partition(running_partition, &running_image_state)) == ESP_ERR_NOT_SUPPORTED) {
                LOGD(TAG, "Currently running partition is no ota partition");
                running_image_state = ESP_OTA_IMG_UNDEFINED;
            }
            else if (ret != ESP_OK) {
                LOGE(TAG, "Failed to get information about running smart module partition");
                goto exit;
            }
            else {
                LOGD(TAG, "Running smart module partition currently is in state: %u", running_image_state);
            }
            if (running_image_state == ESP_OTA_IMG_PENDING_VERIFY) {
                if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                    LOGD(TAG, "Successfully set running partition as new boot partition");
                }
                else {
                    LOGE(TAG, "Failed to set running partition as new boot partition");
                }
            }
            else if (boot_partition->address != running_partition->address) {
                LOGW(TAG, "Smart module boot partition is not same as running partition, boot image is invalid");
            }
            else {
                LOGD(TAG, "Smart module boot partition is same as running partition, boot image is valid");
            }
            if (datastorage_check_file_existence(P_ANKER_UPDATE_STATUS, &file_status) != DATASTORAGE_OK) {
                LOGE(TAG, "Failed to check update status file existence");
                goto exit;
            }
            if (!file_status) {
                LOGD(TAG, "No updates downloaded, skip status check");
                goto exit;
            }
            if (datastorage_read_data_from_file_in_partition(P_ANKER_UPDATE_STATUS, DATASTORAGE_READ_BINARY_DATA, (uint8_t **)&update_info_ptr, &len_of_data) != DATASTORAGE_OK) {
                LOGE(TAG, "Failed to read update status file from partition");
                goto exit;
            }
            if (len_of_data != sizeof(p_anker_update_info_t)) {
                LOGE(TAG, "Failed to read update status file, size invalid");
                goto exit;
            }
            LOGI(TAG, "Update status is %u", update_info.status);
            if (update_info.status == UPDATE_STATUS_INSTALLED) {
                stop_p_anker_poll_mode_and_connect_to_c_anker_ap(update_info.serial);
                initial_done = true;
                return UPDATER_OK;
            }
            else {
                datastorage_erase_file_from_partition(P_ANKER_UPDATE_STATUS);
            }
        }

        exit:
        set_p_anker_into_poll_mode();
        initial_done = true;
    }
    return UPDATER_OK;
}


uint8_t updater_init(device_type_t type) {
    if (type == DEVICE_TYPE_C_ANKER) {
        if (hawkbit_client_init() != HAWKBITCLIENT_OK) {
            LOGD(TAG, "Could not initialize hawkbit client");
            return UPDATER_ERROR;
        }
    }
    else {
	    p_anker_task_event_group = xEventGroupCreate();
    }
    LOGD(TAG, "Updater initialized");

    return UPDATER_OK;
}
