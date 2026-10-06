#include "stdint.h"
#include "c_types.h"
#include "osapi.h"
#include "user_interface.h"
#include "spi_flash.h"
#include "string.h"
#include "user_config.h"
#include "cfg.h"

#define CFG_SECTOR 0x68            /* 0x68000: same place the old firmware used */
#define CFG_MAGIC  0x52505432UL    /* "RPT2" */

static uint32_t ICACHE_FLASH_ATTR cfg_crc(const cfg_t *c)
{
    const uint8_t *p = (const uint8_t *)c;
    uint32_t h = 2166136261UL;
    unsigned i;
    for (i = 0; i < sizeof(cfg_t) - sizeof(uint32_t); i++)
    {
        h ^= p[i];
        h *= 16777619UL;
    }
    return h;
}

void ICACHE_FLASH_ATTR cfg_defaults(cfg_t *c)
{
    os_memset(c, 0, sizeof(cfg_t));
    c->magic = CFG_MAGIC;
    c->length = sizeof(cfg_t);
    os_strcpy(c->ap_ssid, AP_DEFAULT_SSID);
    os_strcpy(c->ap_pass, AP_DEFAULT_PASS);
    c->ap_open = 0;
    c->ap_hidden = 0;
    c->max_clients = 8;
}

void ICACHE_FLASH_ATTR cfg_save(cfg_t *c)
{
    c->magic = CFG_MAGIC;
    c->length = sizeof(cfg_t);
    c->crc = cfg_crc(c);
    spi_flash_erase_sector(CFG_SECTOR);
    spi_flash_write(CFG_SECTOR * SPI_FLASH_SEC_SIZE, (uint32 *)c, sizeof(cfg_t));
}

int ICACHE_FLASH_ATTR cfg_load(cfg_t *c)
{
    int ok = 1;

    spi_flash_read(CFG_SECTOR * SPI_FLASH_SEC_SIZE, (uint32 *)c, sizeof(cfg_t));

    if (c->magic != CFG_MAGIC || c->length != sizeof(cfg_t) || c->crc != cfg_crc(c))
        ok = 0;

    if (ok)
    {
        /* force termination and sane values */
        c->sta_ssid[32] = 0;
        c->sta_pass[64] = 0;
        c->ap_ssid[32] = 0;
        c->ap_pass[64] = 0;
        if (c->ap_ssid[0] == 0)
            ok = 0;
        if (!c->ap_open && os_strlen(c->ap_pass) < 8)
            ok = 0;
        if (c->max_clients < 1 || c->max_clients > 8)
            c->max_clients = 8;
    }

    if (!ok)
    {
        os_printf("config: defaults loaded\r\n");
        cfg_defaults(c);
        cfg_save(c);
    }
    return ok;
}
