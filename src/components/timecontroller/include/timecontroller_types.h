#ifndef TIMECONTROLLER_TYPES_H
#define TIMECONTROLLER_TYPES_H


// Return Codes
#include <stdbool.h>
#include <sys/time.h>

#define TIMCON_OK                        (0)
#define TIMCON_BASE                      (2300)
#define TIMCON_ERR_GENERAL               (TIMCON_BASE + 1)
#define TIMCON_NVS_KEY_NOT_FOUND         (TIMCON_BASE + 2) /*!< Thrown, when the provided NVS-Key was not found and therefore, no data was provided */
#define TIMCON_POINTER_ERR               (TIMCON_BASE + 3) /*!< Thrown, when pointer is NULL */
#define TIMCON_INVALID_VALUE_ERR         (TIMCON_BASE + 4) /*!< Thrown, when the provided value was invalid */
#define TIMCON_OUT_OF_RANGE_ERR          (TIMCON_BASE + 5) /*!< Thrown, when the provided value was out of range */
#define TIMCON_MEMORY_ERR                (TIMCON_BASE + 6) /*!< Thrown, when no memory was left */
#define TIMCON_CLIENT_ERR                (TIMCON_BASE + 7) /*!< Thrown, when client is not initialized */
#define TIMCON_NOT_SYNCED_ERR            (TIMCON_BASE + 8) /*!< Thrown, when client is not initialized */

#define SNTP_ADDRESS                        "pool.ntp.org"     /*!< Address of the SNTP-Server. */


/**
 * @brief Contains information of the current timestamp.
 * */
typedef struct timestamp_status {
	time_t timestamp;       /*!< Timestamp. */
	bool sync_status;       /*!< RTC sync status of timestamp. */
} timestamp_status_t;


typedef enum timestamp_sync_type {
	TIMESTAMP_SYNC_STATUS_UNSYNCED = 0,
	TIMESTAMP_SYNC_STATUS_NRGCP = 1,
	TIMESTAMP_SYNC_STATUS_GPS = 2,
	TIMESTAMP_SYNC_STATUS_SNTP = 3,
} timestamp_sync_type;


// region Callback defintions
typedef int (*timecontroller_nvs_get_tz)(const char *key, uint8_t *value);


typedef int (*timecontroller_nvs_set_tz)(const char *key, uint8_t value);


typedef int (*timecontroller_nvs_get_ts)(const char *key, uint32_t *value);


typedef int (*timecontroller_nvs_set_ts)(const char *key, uint32_t value);


typedef int (*timecontroller_get_ts)(timestamp_status_t *timestamp, bool force_timesave);


typedef int (*timecontroller_rtc_updated_cb)(void);


typedef int (*timecontroller_timezone_updated_cb)(void);
// endregion


extern const char *TIMEZONE_STR[];

/**
 * @brief Possible timezones within the SmartModule.
 * */
typedef enum timcon_timezone {
	timezone_UNKNOWN_TIMEZONE_TYPE = 0,
	timezone_IDLW_m12 = 1,
	timezone_SST_m11 = 2,
	timezone_HST_m10_HDT_m9 = 3,
	timezone_MIT_m9_30 = 4,
	timezone_AKST_m9_AKDT_m8 = 5,
	timezone_PST_m8_PDT_m7 = 6,
	timezone_MST_m7_MDT_m6 = 7,
	timezone_CST_m6_CDT_m5 = 8,
	timezone_EST_m5_EDT_m4 = 9,
	timezone_AST_m4_ADT_m3 = 10,
	timezone_NST_m3_30_NDT_m2_30 = 11,
	timezone_BRT_m3 = 12,
	timezone_NDT_m2_30 = 13, // no standalone
	timezone_BRST_m2 = 14, // brazilian summer-time not in use since 2019
	timezone_AZOT_m1_AZOST_0 = 15,
	timezone_GMT_0 = 16,
	timezone_CET_p1 = 17,
	timezone_CET_p1_CEST_p2 = 18,
	timezone_EAT_p3 = 19,
	timezone_IRST_p3_30 = 20,
	timezone_GST_p4 = 21,
	timezone_IRDT_p4_30 = 22,
	timezone_PKT_p5 = 23,
	timezone_IST_p5_30 = 24,
	timezone_NPT_p5_45 = 25,
	timezone_BIOT_p6 = 26,
	timezone_MMT_p6_30 = 27,
	timezone_THA_p7 = 28,
	timezone_CT_p8 = 29,
	timezone_CWST_p8_45 = 30,
	timezone_JST_p9 = 31,
	timezone_ACST_p9_30 = 32,
	timezone_AEST_p10 = 33,
	timezone_ACST_p9_30_ACDT_p10_30 = 34,
	timezone_AEST_p10_AEDT_p11 = 35,
	timezone_NZST_p12 = 36, // not in use standalone
	timezone_NZST_p12_NZDT_p13 = 37,
	timezone_CHADT_p13_45 = 38,
	timezone_LINT_p14 = 39,
	timezone_WET_0 = 40, // not in use standalone
	timezone_WET_0_WEST_p1 = 41,
	timezone_GMT0_BST_p1 = 42,
	timezone_WAT_p1 = 43,
	timezone_CAT_p2 = 44,
	timezone_SAST_p2 = 45,
	timezone_EET_p2 = 46,
	timezone_EET_p2_t0_EEST_p3_t24 = 47,
	timezone_EET_p2_t0_EEST_p3_t0 = 48,
	timezone_EET_p2_t2_EEST_p3_t3 = 49,
	timezone_EET_p2_t3_EEST_p3_t4 = 50,
	timezone_EET_p2_t50_EEST_p3_t50 = 51,
	timezone_IST_p2_IDT_p3 = 52,
	timezone_MSK_p3 = 53,
	timezone_WIB_p7 = 54,
	timezone_AWST_p8 = 55,
	timezone_CST_p8 = 56,
	timezone_WITA_p8 = 57,
	timezone_PST_p8 = 58,
	timezone_HKT_p8 = 59,
	timezone_WIT_p9 = 60,
	timezone_KST_p9 = 61,
	timezone_ChST_p10 = 62,
	timezone_IST_p1_GMT_0 = 63,
	timezone_AST_m4 = 64,
	timezone_EST_m5 = 65,
	timezone_CST_m5_CDT_m6 = 66,
	timezone_CST_m6 = 67,
	timezone_MST_m7 = 68,
	timezone_HST_m10 = 69,
	timezone_GMT_p01 = 70,
	timezone_GMT_p02 = 71,
	timezone_GMT_p03 = 72,
	timezone_GMT_p0330 = 73,
	timezone_GMT_p04 = 74,
	timezone_GMT_p0430 = 75,
	timezone_GMT_p05 = 76,
	timezone_GMT_p0530 = 77,
	timezone_GMT_p0545 = 78,
	timezone_GMT_p06 = 79,
	timezone_GMT_p0630 = 80,
	timezone_GMT_p07 = 81,
	timezone_GMT_p08 = 82,
	timezone_GMT_p0845 = 83,
	timezone_GMT_p09 = 84,
	timezone_GMT_p10 = 85,
	timezone_GMT_p1030_p11 = 86,
	timezone_GMT_p11 = 87,
	timezone_GMT_p11_p12 = 88,
	timezone_GMT_p12 = 89,
	timezone_GMT_p1245_p1345 = 90,
	timezone_GMT_p13 = 91,
	timezone_GMT_p14 = 92,
	timezone_GMT_0_p02 = 93,
	timezone_GMT_m01 = 94,
	timezone_GMT_m01_0 = 95,
	timezone_GMT_m02_m01 = 96,
	timezone_GMT_m02 = 97,
	timezone_GMT_m03 = 98,
	timezone_GMT_m03_m02 = 99,
	timezone_GMT_m04_m03 = 100,
	timezone_GMT_m04_m03_alt = 101,
	timezone_GMT_m04 = 102,
	timezone_GMT_m05 = 103,
	timezone_GMT_m06 = 104,
	timezone_GMT_m06_m05 = 105,
	timezone_GMT_m07 = 106,
	timezone_GMT_m08 = 107,
	timezone_GMT_m09 = 108,
	timezone_GMT_m0930 = 109,
	timezone_GMT_m10 = 110,
	timezone_GMT_m11 = 111,
	timezone_GMT_m12 = 112,
} timcon_timezone_t;
#define TIMECON_TIMEZONE_MIN     timezone_UNKNOWN_TIMEZONE_TYPE  /*!< Min value of the timezone enum */
#define TIMECON_TIMEZONE_MAX     timezone_GMT_m12                /*!< Max value of the timezone enum */

#endif //TIMECONTROLLER_TYPES_H
