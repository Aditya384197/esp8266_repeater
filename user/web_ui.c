/*
 * Web dashboard: http://192.168.4.1
 *
 *   /            dashboard page (streamed from flash)
 *   /status      JSON status
 *   /scan        start / read WiFi scan
 *   /save        change uplink / AP settings
 *   /reboot      restart,  /factory  reset settings
 *
 * Every reply carries Content-Length, is sent in chunks from the "sent"
 * callback and the connection is closed only after the last chunk.
 * Settings are URL-decoded properly (+ and %xx) and validated before saving.
 */
#include "stdint.h"
#include "c_types.h"
#include "mem.h"
#include "ets_sys.h"
#include "osapi.h"
#include "gpio.h"
#include "os_type.h"
#include "lwip/ip.h"
#include "lwip/netif.h"
#include "lwip/dns.h"
#include "lwip/lwip_napt.h"
#include "lwip/ip_route.h"
#include "lwip/app/dhcpserver.h"
#include "lwip/app/espconn.h"
#include "lwip/app/espconn_tcp.h"
#include "user_interface.h"
#include "string.h"
#include "user_config.h"
#include "config_flash.h"
#include "sys_time.h"
#include "web_ui.h"
#include "web_page.h"

#define WEB_SLOTS     5
#define WEB_CHUNK     1024      /* multiple of 4 (flash is read word-wise) */
#define HDR_MAX       160
#define SCAN_MAX      12

/* the dashboard stays in flash and is streamed in chunks */
static const uint8_t page_str[] ICACHE_RODATA_ATTR STORE_ATTR = WEB_PAGE;

typedef struct
{
    struct espconn *conn;
    char *buf;              /* RAM reply (to free) or chunk buffer for flash replies */
    const uint8_t *data;    /* RAM data start inside buf */
    const uint8_t *fl;      /* flash source (4-byte aligned) or NULL */
    uint16_t len;
    uint16_t off;
} tx_t;

typedef struct
{
    char ssid[33];
    sint8 rssi;
    uint8 ch;
    uint8 secure;
} scan_t;

extern sysconfig_t config;
extern uint8_t web_last_disc_reason;
extern uint64_t Bytes_in, Bytes_out;
extern void user_set_station_config(void);

#define APP_VERSION ESP_REPEATER_VERSION

/* ---- deferred actions: the HTTP reply leaves first, then we act ---- */
static os_timer_t act_timer;
static uint8_t act_pending;
static uint8_t act_phase;

static void ICACHE_FLASH_ATTR act_cb(void *arg)
{
    uint8_t a = act_pending;

    if (a == 1)
    {
        system_restart();
    }
    else if (a == 2)
    {
        if (act_phase == 0)
        {
            user_set_station_config();
            wifi_station_disconnect();
            act_phase = 1;
            os_timer_arm(&act_timer, 500, 0);
        }
        else
        {
            act_phase = 0;
            act_pending = 0;
            wifi_station_connect();
        }
    }
}

static void ICACHE_FLASH_ATTR act_start(uint8_t act, uint32_t ms)
{
    os_timer_disarm(&act_timer);
    act_pending = act;
    act_phase = 0;
    os_timer_setfn(&act_timer, (os_timer_func_t *)act_cb, NULL);
    os_timer_arm(&act_timer, ms ? ms : 1, 0);
}

#define app_restart_later(ms)   act_start(1, ms)
#define app_reconnect_later(ms) act_start(2, ms)

/* speed measurement between two /status calls */
static uint64_t sp_in, sp_out;
static uint32_t sp_t;
static uint32_t sp_down, sp_up;

static tx_t tx[WEB_SLOTS];
static scan_t scan_list[SCAN_MAX];
static uint8_t scan_cnt;
static bool scan_busy;
static uint32_t scan_t0;

/* ------------------------------------------------------------------ */
/* chunked sender                                                      */
/* ------------------------------------------------------------------ */
static void ICACHE_FLASH_ATTR tx_free(struct espconn *c)
{
    int i;
    for (i = 0; i < WEB_SLOTS; i++)
    {
        if (tx[i].conn == c)
        {
            if (tx[i].buf != NULL)
                os_free(tx[i].buf);
            os_memset(&tx[i], 0, sizeof(tx_t));
        }
    }
}

static void ICACHE_FLASH_ATTR tx_next(struct espconn *c)
{
    int i;
    for (i = 0; i < WEB_SLOTS; i++)
    {
        if (tx[i].conn == c)
        {
            uint16_t left = tx[i].len - tx[i].off;
            uint16_t n, off;

            if (left == 0)
            {
                tx_free(c);
                espconn_disconnect(c);
                return;
            }
            n = left > WEB_CHUNK ? WEB_CHUNK : left;
            off = tx[i].off;
            tx[i].off += n;
            if (tx[i].fl != NULL)
            {
                const uint32_t *src = (const uint32_t *)(tx[i].fl + off);
                uint32_t *dst = (uint32_t *)tx[i].buf;
                uint16_t w, words = (n + 3) / 4;
                for (w = 0; w < words; w++)
                    dst[w] = src[w];
                espconn_send(c, (uint8_t *)tx[i].buf, n);
            }
            else
            {
                espconn_send(c, (uint8_t *)(tx[i].data + off), n);
            }
            return;
        }
    }
    espconn_disconnect(c);
}

static tx_t *ICACHE_FLASH_ATTR tx_slot(struct espconn *c)
{
    int i;
    tx_free(c);
    for (i = 0; i < WEB_SLOTS; i++)
    {
        if (tx[i].conn == NULL)
            return &tx[i];
    }
    return NULL;
}

/* buf: malloc'ed, reply = data..data+len. Takes ownership. */
static void ICACHE_FLASH_ATTR send_ram(struct espconn *c, char *buf, const char *data, uint16_t len)
{
    tx_t *t = tx_slot(c);
    if (t == NULL)
    {
        os_free(buf);
        espconn_disconnect(c);
        return;
    }
    t->conn = c;
    t->buf = buf;
    t->data = (const uint8_t *)data;
    t->fl = NULL;
    t->len = len;
    t->off = 0;
    tx_next(c);
}

static void ICACHE_FLASH_ATTR send_flash(struct espconn *c, const uint8_t *fl, uint16_t len)
{
    tx_t *t;
    char *chunk = (char *)os_malloc(WEB_CHUNK + 4);

    if (chunk == NULL)
    {
        espconn_disconnect(c);
        return;
    }
    t = tx_slot(c);
    if (t == NULL)
    {
        os_free(chunk);
        espconn_disconnect(c);
        return;
    }
    t->conn = c;
    t->buf = chunk;
    t->data = NULL;
    t->fl = fl;
    t->len = len;
    t->off = 0;
    tx_next(c);
}

/* JSON reply: body is written at buf+HDR_MAX, header is put in front of it */
static char *ICACHE_FLASH_ATTR json_begin(uint16_t body_max)
{
    return (char *)os_malloc(HDR_MAX + body_max + 1);
}

static void ICACHE_FLASH_ATTR json_end(struct espconn *c, char *buf)
{
    char hdr[HDR_MAX];
    int blen = os_strlen(buf + HDR_MAX);
    int hlen;

    os_sprintf(hdr, "HTTP/1.0 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", blen);
    hlen = os_strlen(hdr);
    os_memcpy(buf + HDR_MAX - hlen, hdr, hlen);
    send_ram(c, buf, buf + HDR_MAX - hlen, (uint16_t)(hlen + blen));
}

static void ICACHE_FLASH_ATTR reply_status_line(struct espconn *c, const char *status)
{
    char *b = (char *)os_malloc(100);
    if (b == NULL)
    {
        espconn_disconnect(c);
        return;
    }
    os_sprintf(b, "HTTP/1.0 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
    send_ram(c, b, b, (uint16_t)os_strlen(b));
}

static void ICACHE_FLASH_ATTR reply_result(struct espconn *c, int ok, int reboot, const char *msg)
{
    char *b = json_begin(200);
    if (b == NULL)
    {
        espconn_disconnect(c);
        return;
    }
    os_sprintf(b + HDR_MAX, "{\"ok\":%d,\"reboot\":%d,\"msg\":\"%s\"}", ok, reboot, msg);
    json_end(c, b);
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */
static int ICACHE_FLASH_ATTR utf8_seq(const uint8_t *s, int max)
{
    uint8_t c = s[0];
    if (c < 0x80)
        return 1;
    if (c >= 0xC2 && c <= 0xDF && max >= 2 && (s[1] & 0xC0) == 0x80)
        return 2;
    if (c >= 0xE0 && c <= 0xEF && max >= 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80)
        return 3;
    if (c >= 0xF0 && c <= 0xF4 && max >= 4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80)
        return 4;
    return 0;
}

/* JSON-escape a string: control characters dropped, invalid UTF-8 -> '?'
   (dst needs 2*max+1 bytes) */
static int ICACHE_FLASH_ATTR jesc(char *dst, const char *src, int max)
{
    int n = 0, i = 0, l, k;
    const uint8_t *s = (const uint8_t *)src;

    while (i < max && s[i] != 0)
    {
        if (s[i] < 0x20 || s[i] == 0x7f)
        {
            i++;
        }
        else if (s[i] == '"' || s[i] == '\\')
        {
            dst[n++] = '\\';
            dst[n++] = (char)s[i++];
        }
        else if (s[i] < 0x80)
        {
            dst[n++] = (char)s[i++];
        }
        else
        {
            l = utf8_seq(s + i, max - i);
            if (l == 0)
            {
                dst[n++] = '?';
                i++;
            }
            else
            {
                for (k = 0; k < l; k++)
                    dst[n++] = (char)s[i++];
            }
        }
    }
    dst[n] = 0;
    return n;
}

static int ICACHE_FLASH_ATTR hexv(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

/* query value, URL-decoded. Returns 0 when the key is absent,
   otherwise decoded length + 1 (value is truncated to outsz-1) */
static int ICACHE_FLASH_ATTR qget(const char *q, const char *key, char *out, int outsz)
{
    int klen = os_strlen(key);
    const char *p = q;

    while (*p)
    {
        if (os_strncmp(p, key, klen) == 0 && p[klen] == '=')
        {
            const char *v = p + klen + 1;
            int n = 0, st = 0;
            while (*v && *v != '&')
            {
                char ch = *v++;
                if (ch == '+')
                {
                    ch = ' ';
                }
                else if (ch == '%' && hexv(v[0]) >= 0 && hexv(v[1]) >= 0)
                {
                    ch = (char)(hexv(v[0]) * 16 + hexv(v[1]));
                    v += 2;
                }
                if (ch == 0)
                    continue;
                n++;
                if (st < outsz - 1)
                    out[st++] = ch;
            }
            out[st] = 0;
            return n + 1;
        }
        while (*p && *p != '&')
            p++;
        if (*p == '&')
            p++;
    }
    out[0] = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* handlers                                                            */
/* ------------------------------------------------------------------ */
static void ICACHE_FLASH_ATTR h_status(struct espconn *c)
{
    char *b = json_begin(760);
    char *o;
    char e1[70], e2[70];
    struct ip_info ipi;
    int up = (wifi_station_get_connect_status() == STATION_GOT_IP);
    int rssi = -100;
    uint32_t ip, gw, now, dt;

    if (b == NULL)
    {
        espconn_disconnect(c);
        return;
    }
    if (up)
    {
        rssi = wifi_station_get_rssi();
        if (rssi >= 0)
            rssi = -100; /* 31 = unknown */
    }
    os_memset(&ipi, 0, sizeof(ipi));
    wifi_get_ip_info(STATION_IF, &ipi);
    ip = ipi.ip.addr;
    gw = ipi.gw.addr;

    now = system_get_time() / 1000; /* ms */
    dt = now - sp_t;
    if (dt >= 1500)
    {
        sp_down = (uint32_t)((Bytes_in - sp_in) * 1000 / dt);
        sp_up = (uint32_t)((Bytes_out - sp_out) * 1000 / dt);
        sp_in = Bytes_in;
        sp_out = Bytes_out;
        sp_t = now;
        if (dt > 20000)
            sp_down = sp_up = 0;
    }

    o = b + HDR_MAX;
    jesc(e1, (const char *)config.ssid, 32);
    jesc(e2, (const char *)config.ap_ssid, 32);
    os_sprintf(o, "{\"cfg\":%d,\"conn\":%d,\"dr\":%d,\"rssi\":%d,\"ch\":%d,\"ssid\":\"%s\",\"pwset\":%d,\"ap_ssid\":\"%s\",\"ap_open\":%d,",
               (config.ssid[0] && os_strcmp((char *)config.ssid, WIFI_SSID) != 0) ? 1 : 0, up, up ? 0 : web_last_disc_reason, rssi, wifi_get_channel(), e1,
               config.password[0] ? 1 : 0, e2, config.ap_open ? 1 : 0);
    o += os_strlen(o);
    os_sprintf(o, "\"ip\":\"%d.%d.%d.%d\",\"gw\":\"%d.%d.%d.%d\",\"clients\":%d,",
               (int)(ip & 0xff), (int)((ip >> 8) & 0xff), (int)((ip >> 16) & 0xff), (int)((ip >> 24) & 0xff),
               (int)(gw & 0xff), (int)((gw >> 8) & 0xff), (int)((gw >> 16) & 0xff), (int)((gw >> 24) & 0xff),
               (int)wifi_softap_get_station_num());
    o += os_strlen(o);
    os_sprintf(o, "\"up\":%d,\"heap\":%d,\"rx\":%d,\"tx\":%d,\"sd\":%d,\"su\":%d,\"ver\":\"%s\"}",
               (int)(get_long_systime() / 1000000), (int)system_get_free_heap_size(),
               (int)(Bytes_in / 1024), (int)(Bytes_out / 1024),
               (int)sp_down, (int)sp_up, APP_VERSION);
    json_end(c, b);
}

static void ICACHE_FLASH_ATTR scan_done(void *arg, STATUS status)
{
    struct bss_info *bss = (struct bss_info *)arg;
    int i, j;

    scan_cnt = 0;
    if (status == OK)
    {
        while (bss != NULL)
        {
            int len = bss->ssid_len;
            if (len > 32)
                len = 32;
            if (len > 0 && bss->ssid[0] != 0)
            {
                char name[33];
                int found = -1;

                os_memcpy(name, bss->ssid, len);
                name[len] = 0;
                for (i = 0; i < scan_cnt; i++)
                {
                    if (os_strcmp(scan_list[i].ssid, name) == 0)
                    {
                        found = i;
                        break;
                    }
                }
                if (found >= 0)
                {
                    if (bss->rssi > scan_list[found].rssi)
                    {
                        scan_list[found].rssi = bss->rssi;
                        scan_list[found].ch = bss->channel;
                        scan_list[found].secure = (bss->authmode != AUTH_OPEN) ? 1 : 0;
                    }
                }
                else if (scan_cnt < SCAN_MAX)
                {
                    os_strcpy(scan_list[scan_cnt].ssid, name);
                    scan_list[scan_cnt].rssi = bss->rssi;
                    scan_list[scan_cnt].ch = bss->channel;
                    scan_list[scan_cnt].secure = (bss->authmode != AUTH_OPEN) ? 1 : 0;
                    scan_cnt++;
                }
            }
            bss = bss->next.stqe_next;
        }
        for (i = 0; i < scan_cnt; i++)
        {
            for (j = i + 1; j < scan_cnt; j++)
            {
                if (scan_list[j].rssi > scan_list[i].rssi)
                {
                    scan_t t;
                    os_memcpy(&t, &scan_list[i], sizeof(t));
                    os_memcpy(&scan_list[i], &scan_list[j], sizeof(t));
                    os_memcpy(&scan_list[j], &t, sizeof(t));
                }
            }
        }
    }
    scan_busy = false;
}

static void ICACHE_FLASH_ATTR h_scan(struct espconn *c, const char *q)
{
    char tmp[4];
    char e[70];
    char *b, *o;
    int i;

    if (scan_busy && (uint32_t)(system_get_time() - scan_t0) > 15000000UL)
        scan_busy = false;

    if (qget(q, "go", tmp, sizeof(tmp)) && !scan_busy)
    {
        scan_busy = true;
        scan_t0 = system_get_time();
        if (!wifi_station_scan(NULL, scan_done))
            scan_busy = false;
    }

    b = json_begin(SCAN_MAX * 110 + 40);
    if (b == NULL)
    {
        espconn_disconnect(c);
        return;
    }
    o = b + HDR_MAX;
    os_sprintf(o, "{\"busy\":%d,\"list\":[", scan_busy ? 1 : 0);
    for (i = 0; i < scan_cnt; i++)
    {
        o += os_strlen(o);
        jesc(e, scan_list[i].ssid, 32);
        os_sprintf(o, "%s{\"s\":\"%s\",\"r\":%d,\"c\":%d,\"a\":%d}", i ? "," : "", e,
                   (int)scan_list[i].rssi, (int)scan_list[i].ch, (int)scan_list[i].secure);
    }
    o += os_strlen(o);
    os_sprintf(o, "]}");
    json_end(c, b);
}

static void ICACHE_FLASH_ATTR h_save(struct espconn *c, const char *q)
{
    char s_ssid[40], s_pw[80], a_ssid[40], a_pw[80], sec[16], act[16], opn[4];
    int l_ss, l_sp, l_as, l_ap, l_sec, sta_open, want_open, eff;
    int sta_changed = 0, ap_changed = 0;

    l_ss = qget(q, "sta_ssid", s_ssid, sizeof(s_ssid));
    l_sp = qget(q, "sta_pw", s_pw, sizeof(s_pw));
    l_as = qget(q, "ap_ssid", a_ssid, sizeof(a_ssid));
    l_ap = qget(q, "ap_pw", a_pw, sizeof(a_pw));
    l_sec = qget(q, "ap_sec", sec, sizeof(sec));
    sta_open = qget(q, "sta_open", opn, sizeof(opn)) ? 1 : 0;
    qget(q, "act", act, sizeof(act));

    /* ---- validate first, change nothing on error ---- */
    if (l_ss && l_ss - 1 > 32)
    {
        reply_result(c, 0, 0, "Uplink SSID: max 32 characters");
        return;
    }
    if (l_sp > 1 && (l_sp - 1 < 8 || l_sp - 1 > 63))
    {
        reply_result(c, 0, 0, "Uplink password must be 8-63 characters");
        return;
    }
    if (l_as && (l_as - 1 < 1 || l_as - 1 > 32))
    {
        reply_result(c, 0, 0, "AP SSID must be 1-32 characters");
        return;
    }
    if (l_ap > 1 && (l_ap - 1 < 8 || l_ap - 1 > 63))
    {
        reply_result(c, 0, 0, "AP password must be 8-63 characters");
        return;
    }
    want_open = config.ap_open ? 1 : 0;
    if (l_sec)
        want_open = (os_strcmp(sec, "open") == 0) ? 1 : 0;
    eff = (l_ap > 1) ? (l_ap - 1) : (int)os_strlen((char *)config.ap_password);
    if (!want_open && eff < 8)
    {
        reply_result(c, 0, 0, "AP password must be 8-63 characters (or choose Open)");
        return;
    }

    /* ---- apply ---- */
    if (l_ss && os_strcmp(s_ssid, (char *)config.ssid) != 0)
    {
        os_memset(config.ssid, 0, sizeof(config.ssid));
        os_memcpy(config.ssid, s_ssid, l_ss - 1);
        os_memset(config.bssid, 0, sizeof(config.bssid)); /* never lock to one BSSID: survives channel changes */
        config.automesh_mode = AUTOMESH_OFF;
        if (l_sp <= 1 && !sta_open)
            os_memset(config.password, 0, sizeof(config.password)); /* new network: old password is useless */
        sta_changed = 1;
    }
    if (sta_open && config.password[0] != 0)
    {
        os_memset(config.password, 0, sizeof(config.password));
        sta_changed = 1;
    }
    if (l_sp > 1 && os_strcmp(s_pw, (char *)config.password) != 0)
    {
        os_memset(config.password, 0, sizeof(config.password));
        os_memcpy(config.password, s_pw, l_sp - 1);
        sta_changed = 1;
    }
    if (l_as && os_strcmp(a_ssid, (char *)config.ap_ssid) != 0)
    {
        os_memset(config.ap_ssid, 0, sizeof(config.ap_ssid));
        os_memcpy(config.ap_ssid, a_ssid, l_as - 1);
        ap_changed = 1;
    }
    if (l_ap > 1 && os_strcmp(a_pw, (char *)config.ap_password) != 0)
    {
        os_memset(config.ap_password, 0, sizeof(config.ap_password));
        os_memcpy(config.ap_password, a_pw, l_ap - 1);
        ap_changed = 1;
    }
    if ((config.ap_open ? 1 : 0) != want_open)
    {
        config.ap_open = want_open;
        ap_changed = 1;
    }
    config_save(&config);

    if (os_strcmp(act, "reboot") == 0)
    {
        reply_result(c, 1, 1, "Saved. Restarting...");
        app_restart_later(1200);
    }
    else if (os_strcmp(act, "connect") == 0)
    {
        reply_result(c, 1, 0, ap_changed ? "Saved. Connecting... (AP changes apply after restart)" : "Saved. Connecting to router...");
        app_reconnect_later(800);
    }
    else
    {
        reply_result(c, 1, 0, ap_changed ? "Saved. Restart to apply AP changes" : (sta_changed ? "Saved. Use Save & Connect to apply" : "Saved"));
    }
}

/* ------------------------------------------------------------------ */
/* espconn callbacks                                                   */
/* ------------------------------------------------------------------ */
static void ICACHE_FLASH_ATTR web_recv(void *arg, char *data, unsigned short length)
{
    struct espconn *c = (struct espconn *)arg;
    char req[200];
    char *q;
    int i = 4, n = 0;

    if (length < 6 || os_strncmp(data, "GET ", 4) != 0)
    {
        espconn_disconnect(c);
        return;
    }
    while (i < length && data[i] != ' ' && data[i] != '\r' && data[i] != '\n' && n < (int)sizeof(req) - 1)
        req[n++] = data[i++];
    req[n] = 0;
    if (i >= length || data[i] != ' ')
    {
        reply_status_line(c, "400 Bad Request");
        return;
    }

    q = req;
    while (*q && *q != '?')
        q++;
    if (*q == '?')
    {
        *q = 0;
        q++;
    }

    if (os_strcmp(req, "/") == 0 || os_strcmp(req, "/index.html") == 0)
    {
        send_flash(c, page_str, (uint16_t)(sizeof(page_str) - 1));
    }
    else if (os_strcmp(req, "/status") == 0)
    {
        h_status(c);
    }
    else if (os_strcmp(req, "/save") == 0)
    {
        h_save(c, q);
    }
    else if (os_strcmp(req, "/scan") == 0)
    {
        h_scan(c, q);
    }
    else if (os_strcmp(req, "/reboot") == 0)
    {
        reply_result(c, 1, 1, "Restarting...");
        app_restart_later(1200);
    }
    else if (os_strcmp(req, "/factory") == 0)
    {
        config_load_default(&config);
        config_save(&config);
        reply_result(c, 1, 1, "Factory reset done. Restarting...");
        app_restart_later(1200);
    }
    else
    {
        reply_status_line(c, "404 Not Found");
    }
}

void ICACHE_FLASH_ATTR web_ui_recv(void *arg, char *data, unsigned short length)
{
    web_recv(arg, data, length);
}

void ICACHE_FLASH_ATTR web_ui_sent(void *arg)
{
    tx_next((struct espconn *)arg);
}

void ICACHE_FLASH_ATTR web_ui_discon(void *arg)
{
    tx_free((struct espconn *)arg);
}
