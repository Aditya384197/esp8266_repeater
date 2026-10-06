#ifndef _USER_CONFIG_H_
#define _USER_CONFIG_H_

#define APP_VERSION        "R1.0"

/* default access point (used on first boot and after factory reset) */
#define AP_DEFAULT_SSID    "ESP-Repeater"
#define AP_DEFAULT_PASS    "12345678"

/* the SDK in the build image already provides user_rf_cal_sector_set() */
#define USER_RF_CAL        0

#endif /* _USER_CONFIG_H_ */
