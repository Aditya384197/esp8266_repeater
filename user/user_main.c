/*
 * ESP8266 WiFi repeater (NAT) - lean rewrite
 *
 *   router  <--WiFi-->  [ESP8266 STA | NAT | AP]  <--WiFi-->  your devices
 *
 * - The access point always lives at 192.168.4.1 (dashboard: http://192.168.4.1)
 * - Clients get their address from the ESP's own DHCP server, traffic is NAT'ed
 * - The AP is NEVER restarted while running. When the uplink drops or the router
 *   changes its channel only the station side reconnects, clients stay associated.
 * - Own watchdog re-connects the uplink if it stays down.
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
#include "app.h"


cfg_t cfg;
uint8_t app_last_disc_reason;
uint64_t app_bytes_down, app_bytes_up;
uint32_t app_speed_down, app_speed_up;
uint32_t app_uptime_s;

static uint64_t last_down, last_up;
static bool ap_ip_done;
static uint8_t uplink_down_s;
static uint8_t button_s;
static uint8_t pending_action;
static os_timer_t tick_timer;
static os_timer_t action_timer;

#define ACT_RESTART      1
#define ACT_DISCONNECT   2
#define ACT_CONNECT      3

/* ------------------------------------------------------------------ */
/* Traffic counters: thin hooks on the station netif (counting only)    */
/* ------------------------------------------------------------------ */
static netif_input_fn orig_input_sta;
static netif_linkoutput_fn orig_output_sta;

static err_t hook_input_sta(struct pbuf *p, struct netif *inp)
{
    app_bytes_down += p->tot_len;
    return orig_input_sta(p, inp);
}

static err_t hook_output_sta(struct netif *outp, struct pbuf *p)
{
    app_bytes_up += p->tot_len;
    return orig_output_sta(outp, p);
}

static struct netif *ICACHE_FLASH_ATTR find_netif(uint32_t addr)
{
    struct netif *nif;
    for (nif = netif_list; nif != NULL; nif = nif->next)
    {
        if (nif->ip_addr.addr == addr)
            return nif;
    }
    return NULL;
}

/* NAT must be switched on for the AP side netif */
static void ICACHE_FLASH_ATTR enable_nat_on_ap(void)
{
    ip_addr_t ap_ip;
    struct netif *nif;

    IP4_ADDR(&ap_ip, 192, 168, 4, 1);
    nif = find_netif(ap_ip.addr);
    if (nif != NULL)
        nif->napt = 1;
}

static void ICACHE_FLASH_ATTR hook_sta_netif(uint32_t sta_ip)
{
    struct netif *nif = find_netif(sta_ip);

    if (nif == NULL)
        return;
    /* install only once, never wrap our own hook (that would recurse) */
    if (nif->input != hook_input_sta)
    {
        orig_input_sta = nif->input;
        nif->input = hook_input_sta;
    }
    if (nif->linkoutput != hook_output_sta)
    {
        orig_output_sta = nif->linkoutput;
        nif->linkoutput = hook_output_sta;
    }
}

/* ------------------------------------------------------------------ */
/* WiFi configuration                                                  */
/* ------------------------------------------------------------------ */
static void ICACHE_FLASH_ATTR sta_apply_config(void)
{
    struct station_config sc;

    os_memset(&sc, 0, sizeof(sc));
    os_memcpy(sc.ssid, cfg.sta_ssid, os_strlen(cfg.sta_ssid));
    os_memcpy(sc.password, cfg.sta_pass, os_strlen(cfg.sta_pass));
    sc.bssid_set = 0; /* never lock to one BSSID: survives channel / AP changes */
    wifi_station_set_config(&sc);
    wifi_station_set_hostname("ESP-Repeater");
}

static void ICACHE_FLASH_ATTR ap_apply_config(void)
{
    struct softap_config ac;

    wifi_softap_get_config(&ac);
    os_memset(ac.ssid, 0, sizeof(ac.ssid));
    os_memcpy(ac.ssid, cfg.ap_ssid, os_strlen(cfg.ap_ssid));
    ac.ssid_len = (uint8)os_strlen(cfg.ap_ssid);
    os_memset(ac.password, 0, sizeof(ac.password));
    if (cfg.ap_open)
    {
        ac.authmode = AUTH_OPEN;
    }
    else
    {
        os_memcpy(ac.password, cfg.ap_pass, os_strlen(cfg.ap_pass));
        ac.authmode = AUTH_WPA_WPA2_PSK;
    }
    ac.max_connection = cfg.max_clients;
    ac.ssid_hidden = cfg.ap_hidden;
    ac.beacon_interval = 100;
    wifi_softap_set_config(&ac);
}

/* AP subnet + DHCP server (done once, after the netif exists) */
static void ICACHE_FLASH_ATTR ap_ip_config(void)
{
    struct ip_info info;
    struct dhcps_lease lease;
    struct netif *nif;
    ip_addr_t dns;

    /* the AP netif is the first one that is not number 0 */
    for (nif = netif_list; nif != NULL && nif->num == 0; nif = nif->next)
        ;
    if (nif == NULL)
        return;
    /* Espressif internals expect the SoftAP netif number to be 1 */
    nif->num = 1;

    wifi_softap_dhcps_stop();

    IP4_ADDR(&info.ip, 192, 168, 4, 1);
    info.gw = info.ip;
    IP4_ADDR(&info.netmask, 255, 255, 255, 0);
    wifi_set_ip_info(nif->num, &info);

    wifi_softap_dhcps_stop();
    IP4_ADDR(&lease.start_ip, 192, 168, 4, 2);
    IP4_ADDR(&lease.end_ip, 192, 168, 4, 100);
    wifi_softap_set_dhcps_lease(&lease);
    wifi_softap_set_dhcps_lease_time(120); /* minutes */
    wifi_softap_dhcps_start();

    IP4_ADDR(&dns, 8, 8, 8, 8);
    dhcps_set_DNS(&dns);

    enable_nat_on_ap();
    ap_ip_done = true;
}

static void ICACHE_FLASH_ATTR wifi_event_cb(System_Event_t *evt)
{
    ip_addr_t dns;

    switch (evt->event)
    {
    case EVENT_STAMODE_CONNECTED:
        os_printf("uplink: connected, channel %d\r\n", evt->event_info.connected.channel);
        break;

    case EVENT_STAMODE_DISCONNECTED:
        app_last_disc_reason = evt->event_info.disconnected.reason;
        os_printf("uplink: disconnected, reason %d\r\n", evt->event_info.disconnected.reason);
        break;

    case EVENT_STAMODE_GOT_IP:
        app_last_disc_reason = 0;
        uplink_down_s = 0;
        dns = dns_getserver(0);
        if (dns.addr == 0)
            IP4_ADDR(&dns, 8, 8, 8, 8);
        dhcps_set_DNS(&dns);
        hook_sta_netif(evt->event_info.got_ip.ip.addr);
        enable_nat_on_ap();
        os_printf("uplink: ip " IPSTR " gw " IPSTR "\r\n", IP2STR(&evt->event_info.got_ip.ip), IP2STR(&evt->event_info.got_ip.gw));
        break;

    case EVENT_SOFTAPMODE_STACONNECTED:
    case EVENT_SOFTAPMODE_STADISCONNECTED:
        enable_nat_on_ap();
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Deferred actions                                                    */
/* ------------------------------------------------------------------ */
static void ICACHE_FLASH_ATTR action_cb(void *arg)
{
    uint8_t a = pending_action;
    pending_action = 0;

    if (a == ACT_RESTART)
    {
        system_restart();
    }
    else if (a == ACT_DISCONNECT)
    {
        if (cfg.sta_ssid[0] == 0)
        {
            wifi_station_disconnect();
            return;
        }
        sta_apply_config();
        wifi_station_disconnect();
        pending_action = ACT_CONNECT;
        os_timer_arm(&action_timer, 500, 0);
    }
    else if (a == ACT_CONNECT)
    {
        wifi_station_connect();
    }
}

static void ICACHE_FLASH_ATTR start_action(uint8_t act, uint32_t ms)
{
    os_timer_disarm(&action_timer);
    pending_action = act;
    os_timer_setfn(&action_timer, (os_timer_func_t *)action_cb, NULL);
    os_timer_arm(&action_timer, ms ? ms : 1, 0);
}

void ICACHE_FLASH_ATTR app_reconnect_later(uint32_t ms)
{
    start_action(ACT_DISCONNECT, ms);
}

void ICACHE_FLASH_ATTR app_restart_later(uint32_t ms)
{
    start_action(ACT_RESTART, ms);
}

/* ------------------------------------------------------------------ */
/* 1 second tick: speed, AP setup retry, uplink watchdog, reset button  */
/* ------------------------------------------------------------------ */
static void ICACHE_FLASH_ATTR tick_cb(void *arg)
{
    app_uptime_s++;

    app_speed_down = (uint32_t)(app_bytes_down - last_down);
    app_speed_up = (uint32_t)(app_bytes_up - last_up);
    last_down = app_bytes_down;
    last_up = app_bytes_up;

    if (!ap_ip_done)
        ap_ip_config();

    /* uplink watchdog: if there is no IP for 25 s, restart the station connection */
    if (cfg.sta_ssid[0] != 0 && pending_action == 0)
    {
        if (wifi_station_get_connect_status() == STATION_GOT_IP)
        {
            uplink_down_s = 0;
        }
        else if (++uplink_down_s >= 25)
        {
            uplink_down_s = 0;
            os_printf("uplink watchdog: reconnecting\r\n");
            app_reconnect_later(10);
        }
    }

    /* hold the FLASH button (GPIO0) for 5 s: factory reset */
    if (GPIO_INPUT_GET(0) == 0)
    {
        if (++button_s >= 5)
        {
            cfg_defaults(&cfg);
            cfg_save(&cfg);
            system_restart();
        }
    }
    else
    {
        button_s = 0;
    }
}

static void ICACHE_FLASH_ATTR init_done_cb(void)
{
    web_start();
    if (cfg.sta_ssid[0] != 0)
        wifi_station_connect();

    os_timer_disarm(&tick_timer);
    os_timer_setfn(&tick_timer, (os_timer_func_t *)tick_cb, NULL);
    os_timer_arm(&tick_timer, 1000, 1);
}

void ICACHE_FLASH_ATTR user_init(void)
{
    uart_div_modify(0, 80000000UL / 115200);
    os_printf("\r\n\r\nESP8266 WiFi repeater %s\r\n", APP_VERSION);

    gpio_init();
    system_update_cpu_freq(160);

    cfg_load(&cfg);

    ip_napt_init(IP_NAPT_MAX, IP_PORTMAP_MAX);

    wifi_set_opmode(STATIONAP_MODE);
    wifi_set_phy_mode(PHY_MODE_11N);
    wifi_set_sleep_type(NONE_SLEEP_T);   /* no modem sleep: full speed, no latency spikes */
    wifi_set_event_handler_cb(wifi_event_cb);

    ap_apply_config();
    if (cfg.sta_ssid[0] != 0)
        sta_apply_config();
    wifi_station_set_auto_connect(cfg.sta_ssid[0] != 0 ? 1 : 0);

    system_init_done_cb(init_done_cb);
}
