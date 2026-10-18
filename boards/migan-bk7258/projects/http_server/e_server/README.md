# e_server — host server simulator (ESP32-P4 CB V3.2 API)

A **host-side simulator** of the `app/board_server` embedded web backend.
It serves the exact same single-page frontend and implements the same JSON
API as the firmware, so the site can be developed/debugged in a browser
without the board:

```bash
python build_web.py      # bundle web/ + public/ -> web_assets.h
make                     # or: bash build.sh   (needs gcc/clang)
./e_server 8080          # then open http://localhost:8080/
```

Simulated hardware (mirrors the real board API):

- `GET/POST /api/leds` — `{"leds":[led0,led1]}` (LED0 green / LED1 blue)
- `GET /api/adc` — `{"temp_c":..}` (simulated ~28.5 °C wave)
- `GET /api/camera` — `{"source":"ov5647","ready":1,"w":1024,"h":600,...}`
  (frame counter simulated at ~20 fps; no actual MJPEG on the host)
- `GET /api/info` — `arch: esp32p4` + LAN IP, public/geo/weather null
- `/` — the bundled page; `/public/*` — board photos

`web/` + `public/` here are the **source of truth for the frontend**. After
editing them, regenerate the firmware's copy too:

```bash
python build_web.py --out ../app/board_server/main/web_assets.h
```

(The firmware does not bundle this folder; it embeds the generated header.)
