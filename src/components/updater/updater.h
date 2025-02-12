#ifndef UPDATER_H
#define UPDATER_H


#include <stdint.h>
#include "api.h"


#define UPDATER_OK 0
#define UPDATER_ERROR 1


uint8_t handle_p_anker_updates(uint8_t *cnt_to_update, uint8_t *cnt_joined, uint8_t *cnt_updated);


uint8_t updater_stop(device_type_t type);


uint8_t updater_start(device_type_t type);


uint8_t updater_init(device_type_t type);


#endif
