#ifndef _CFG_H_
#define _CFG_H_

#include "stdint.h"

/* Persistent settings, one flash sector. Size must be a multiple of 4. */
typedef struct
{
    uint32_t magic;
    uint32_t length;
    char sta_ssid[33];  /* uplink (your router); empty = not configured */
    char sta_pass[65];  /* empty = open network */
    char ap_ssid[33];   /* repeater access point */
    char ap_pass[65];   /* WPA2 password (8..63) */
    uint8_t ap_open;    /* 1 = no password */
    uint8_t ap_hidden;
    uint8_t max_clients;
    uint8_t reserved;
    uint32_t crc;
} cfg_t;

void cfg_defaults(cfg_t *c);
int cfg_load(cfg_t *c);   /* returns 1 if a valid config was found, 0 if defaults were loaded */
void cfg_save(cfg_t *c);

#endif /* _CFG_H_ */
