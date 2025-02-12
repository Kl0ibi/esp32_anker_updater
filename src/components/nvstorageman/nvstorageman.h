#ifndef NVSTORAGEMAN_H
#define NVSTORAGEMAN_H


#define NVSM_MAX_KEY_SIZE 17


int nvsm_get_str(const char *key, char *out_value, size_t *length);


int nvsm_set_str(const char *key, const char *value);


int nvsm_get_u8(const char *key, uint8_t *out_value);


int nvsm_set_u8(const char *key, uint8_t value);


int nvsm_get_i8(const char *key, int8_t *out_value);


int nvsm_set_i8(const char *key, int8_t value);


__attribute__((unused)) __attribute__((unused)) int nvsm_get_i16(const char *key, int16_t *out_value);


__attribute__((unused)) __attribute__((unused)) int nvsm_set_i16(const char *key, int16_t value);


__attribute__((unused)) __attribute__((unused)) int nvsm_get_u16(const char *key, uint16_t *out_value);


__attribute__((unused)) __attribute__((unused)) int nvsm_set_u16(const char *key, uint16_t value);


int nvsm_get_i32(const char *key, int32_t *out_value);


int nvsm_set_i32(const char *key, int32_t value);


int nvsm_get_u32(const char *key, uint32_t *out_value);


int nvsm_set_u32(const char *key, uint32_t value);


__attribute__((unused)) __attribute__((unused)) int nvsm_get_i64(const char *key, int64_t *out_value);


__attribute__((unused)) __attribute__((unused)) int nvsm_set_i64(const char *key, int64_t value);


__attribute__((unused)) int nvsm_set_i64(const char *key, int64_t value);


int nvsm_get_u64( const char *key, uint64_t *out_value);


int nvsm_set_u64( const char *key, uint64_t value);


int nvsm_set_float(const char *key, float value);


int nvsm_get_float(const char *key, float *out_value);


/**
 * @brief Initialize Non-volatile storage manager
 *
 * Call this function before any other function in this library.
 *
 * @return
 */
int nvsm_init();


/**
 * @brief Deinitialize Non-volatile storage manager
 *
 * Call this function to close NVS
 *
 * @return
 * 0 - OK
 * 1 - ERROR
 */
int nvsm_deinit();


#endif
