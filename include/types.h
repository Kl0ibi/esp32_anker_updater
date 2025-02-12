# ifndef TYPES_H
# define TYPES_H


#define SERIAL_SIZE 9
#define HB_TOKEN_SIZE 33
#define MAX_VERSION_SIZE 12
#define AP_SSID_SIZE 32
#define AP_PASSWORD_SIZE 32
#define MAX_P_ANKER_TO_UPDATE (2)

typedef struct {
    char version[MAX_VERSION_SIZE];
} software_version_t;


typedef struct {
    char serial[SERIAL_SIZE];
} serial_number_t;


typedef struct {
    char token[HB_TOKEN_SIZE];
} hb_security_token_t;


#endif
