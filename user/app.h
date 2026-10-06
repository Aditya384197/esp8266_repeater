#ifndef _APP_H_
#define _APP_H_

#include "stdint.h"
#include "cfg.h"

extern cfg_t cfg;
extern uint8_t app_last_disc_reason;
extern uint64_t app_bytes_down, app_bytes_up;
extern uint32_t app_speed_down, app_speed_up;   /* bytes per second */
extern uint32_t app_uptime_s;

/* deferred actions (run after the HTTP reply has been sent) */
void app_reconnect_later(uint32_t ms);
void app_restart_later(uint32_t ms);

/* web dashboard */
void web_start(void);

#endif /* _APP_H_ */
