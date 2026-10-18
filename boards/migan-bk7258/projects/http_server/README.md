# http_server

Embedded web server demo for the migan-bk7258, ported from
`esp32-demo/p4-cb-v3_2/app/board_server` (the PC-side API is mirrored by
the copied `e_server/` simulator in this folder).

```
http://192.168.5.101/          (IP printed on the console at boot)
```

## Structure

| path | purpose |
|------|---------|
| `main/` | firmware: WiFi station + lwIP-socket HTTP server + sensors |
| `main/web_assets.h` | the single-page app (gzip) + board photos, generated |
| `e_server/` | copied host simulator (same JSON API, PC debugging) |
| `main/wifi_config.h` | credentials (gitignored) |

The frontend is the reference single-page app with minimal adaptation:
LED names → `LED1 (P40)` / `LED2 (P41)`, camera name → GC2145, board
text → migan-bk7258. After editing `e_server/web/`, regenerate the
bundle:

```
cd e_server && python build_web.py     # -> e_server/web_assets.h
cp web_assets.h ../main/web_assets.h
```

## API (same contract as the reference)

| endpoint | response |
|----------|----------|
| `GET /` | gzip single page (`Content-Encoding: gzip`) |
| `GET/POST /api/leds` | `{"leds":[led1,led2]}` — LED1 = P40, LED2 = P41, high active |
| `GET /api/adc` | `{"temp_c":..,"vbat_v":..}` — die temp (SARADC) + VBAT |
| `GET /api/camera` | `{"source":"gc2145","ready":N,"w":0,"h":0,"frames":0}` |
| `GET /api/camera/control?rot=&quality=` | `{"rot":..,"quality":..}` (echo) |
| `GET /api/info` | `{"arch":"bk7258","lan_ip":..,"public_ip":null,"geo":null,"weather":null}` |
| `GET /stream`, `/capture` | 503 until the GC2145 video pipeline is wired |
| `GET /public/board_X.jpg` | embedded images |

## Notes

- The g-sensor is deliberately left out (not operational on this board).
- The die temperature is the uncalibrated SARADC value scaled as
  raw/10 °C (this chip's OTP lacks the ADC calibration); it tracks the
  die but is not lab-accurate. VBAT is reported in volts from the
  internal channel.
- **Camera**: presence is probed at boot over the camera I2C bus
  (P42 = SCL, P43 = SDA) at GC2145 addresses 0x3C/0x3D. With the module
  not attached the probe reports `not detected` and the page shows the
  camera as not ready (the stream endpoints answer 503, which the UI
  handles gracefully). The MJPEG pipeline (DVP + HW JPEG encoder) is the
  next step once the module is connected.
- The WiFi stack is initialized by `bk_init()`; the app sets the station
  config and starts the HTTP server after DHCP completes.
- Verified on hardware: page + gzip, LEDs, temp/vbat, info, photos,
  graceful 503s. Booting prints `http_server ready: http://<ip>/`.
