# wifi_con_test

Simple WiFi station connection test for the migan-bk7258, behavior
mirrored from `esp32-demo/p4-cb-v3_2/app/wifi_con_test`:

1. scan and list the nearby APs (SSID / BSSID / RSSI / channel / security)
2. connect to `DEFAULT_AP` (30 s timeout)
3. print the acquired IP / gateway / netmask
4. heartbeat LED1 (P40, high active) every 500 ms + a
   `WiFi status: UP/DOWN` line (SSID, RSSI, channel, IP) every 30 s

## How it differs from the ESP32 reference

- The BK7258 has the radio on-chip: `bk_init()` already runs
  `bk_event_init()` + `bk_netif_init()` + `bk_wifi_init()` when
  `CONFIG_WIFI_ENABLE` (default on), and the netif layer starts DHCP
  automatically on `EVENT_WIFI_STA_CONNECTED`. The app only sets the
  station config (`bk_wifi_sta_set_config`) and starts it
  (`bk_wifi_sta_start`).
- AP listing uses the SDK's `bk_wifi_scan_dump_result()`, which prints
  the same table (SSID/BSSID/RSSI/chan/security).
- No separate WPA2 / WPA-mixed retry: the SDK negotiates the security
  from the AP beacon and the configured password (WPA2/WPA3).
- Auto-reconnect is left at the driver default (`auto_reconnect_count=0`
  = always).

## Credentials

```
cp main/wifi_config.h.example main/wifi_config.h
# edit DEFAULT_AP / DEFAULT_PASSWD
```

`main/wifi_config.h` is gitignored.

## Build / flash

```
cd D:/armino/beken-prj
make build BOARD=migan-bk7258 PROJECT=wifi_con_test

bk_loader.exe download -p 42 -b 1500000 ^
    -i D:/armino/beken-prj/build/migan-bk7258/wifi_con_test/bk7258/all-app.bin -s 0
```

Only the cp0 image is needed; cp1/cp2 idle.

## Verified so far

- Scan: `scan found 4 AP` with the full table (HW radio + scan path OK)
- Connect: fails on the placeholder credentials and auto-retries every
  few seconds (expected until real credentials are set)
