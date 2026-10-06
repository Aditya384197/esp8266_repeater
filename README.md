# ESP8266 WiFi Repeater (NAT) - lean rewrite

```
router  <--WiFi-->  [ ESP8266:  STA | NAT | AP ]  <--WiFi-->  your devices
```

Dashboard: **http://192.168.4.1** (connect to the repeater's WiFi first)

Default access point: `ESP-Repeater` / password `12345678` (change it in the dashboard).

## Features
- Uplink SSID/password, scan for networks, Save / Save & Connect / Restart / Factory reset
- Live status: connected state + last disconnect reason, RSSI (dBm, %), quality, approx. distance, channel, IP, clients, live speed, totals, uptime, free heap
- The AP is never restarted while running: if the router drops or changes channel only the uplink reconnects (clients stay associated); uplink watchdog reconnects if it stays down for 25 s
- No BSSID lock, so the repeater follows the router across channel changes
- Hold the FLASH button (GPIO0) for 5 s = factory reset

## Build
Push to GitHub. `.github/workflows/build.yml` builds the firmware and commits it into `firmware/`.

Flash the single image at 0x0:

    esptool.py write_flash -ff 40m -fm dio -fs 32m 0x0 firmware/ESP8266_Repeater_FULL_flash_at_0x0.bin

(for 4 MB flash; use `-fs 8m`/`16m` etc. for smaller chips only if the 4 MB layout does not apply.)

## Source layout
| file | purpose |
|---|---|
| `user/user_main.c` | WiFi setup, NAT, DHCP, events, watchdog, traffic counters |
| `user/web.c`, `user/web_page.h` | HTTP dashboard (page generated from `tools/page.html`) |
| `user/cfg.c`, `user/cfg.h` | settings in flash (CRC protected) |
| `user/rf_init.c` | RF calibration / init data required by the SDK |
| `tools/` | page source and generator (`python3 tools/gen.py tools/page.html user/web_page.h`) |

NAT/DHCP use the `liblwip_open_napt.a` library from Martin Ger's esp_wifi_repeater (GPL), bootloader `rboot.bin` (see `LICENSE_rboot.txt`).

## Expectations
An ESP8266 NAT repeater reaches roughly 2-4 Mbit/s in practice. Use a good 5 V supply (>= 1 A, short cable, 100-470 uF capacitor close to the module): weak power is the most common cause of random speed drops and reconnects.
