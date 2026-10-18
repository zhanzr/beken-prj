# migan-bk7258 — peripheral bring-up status

Status of the on-board peripherals in this project tree. Three
peripherals (g-sensor, camera, SD-NAND) are **not yet working** in our
projects; the software bring-up followed the vendor references and the
investigation is ongoing. Everything else on the board is verified
working.

## Working

| peripheral | project | status |
|------------|---------|--------|
| Dual GC9D01 round LCDs (SW bit-bang + HW QSPI 60 MHz) | `dual_GC9D01_160x160` | working, ~25 ms/frame (HW) |
| Voice recorder (on-chip codec mic + speaker, 16 kHz) | `rec_play` | record + playback verified |
| WiFi station (scan, connect, DHCP) | `wifi_con_test` | connected, IP via DHCP |
| Web server (LEDs, DTS temp, VBAT) | `http_server` | live at board IP |
| SD-NAND / FATFS test app | `nand_fs_test` | app runs; chip not yet responding (see below) |

## Not yet working — g-sensor SC7A20HTR (I2C0, 0x18)

Details: `sc7a20htr_test/README.md`.

The vendor driver + CLI are implemented and the bring-up follows the
vendor references. Observations:

- the bus verifies healthy (idle levels high, external 4.7K pull-ups,
  G_VDD rail alive)
- the chip answered **once** with the correct WHO_AM_I (0x11 at 0x18)
  and has not responded since, across cold power cycles
- `bk_gpio_pull_down()` is a no-op on this SDK (writes pull_mode=0) —
  use `bk_gpio_set_config` with `GPIO_PULL_DOWN_EN` when a real
  pull-down is needed

Open items: re-verify the exact SDO strap and the internal-pullup
setting (DIG_CTRL 0x57 SDO_PU), probe again after a full power cycle,
compare against the vendor wanson/gsensor task init order.

## Not yet working — camera GC2145 (DVP)

The vendor's complete bring-up was implemented and runs
(`bk_dvp_camera_enumerate()`: power P49 → DVP GPIO init → MCLK enable →
I2C init → all 6 sensor detects, 58 s of probing) but reports no
camera. The genie project (all 3 cores flashed) also never completes
`media_app_camera_open` — it blocks in the cp0→cp1 media handshake, so
even the vendor project does not exercise the camera end-to-end as-is.

Observations:

- camera power P49 (DVP_PWR_CTRL → camera LDOs) is driven before probing
- MCLK is enabled by the vendor flow; the DVP data/sync pads map cleanly
- the camera SCCB bus was probed on **both** candidate pin sets
  (P42/P43 and P20/P21) at addresses 0x3C/0x3D/0x21/0x20 — no ACK
- genie runs the camera on cp1 with `CONFIG_SIM_I2C=y` (bit-bang I2C on
  P42/P43) — replicating this exactly in a cp0 project is one open item;
  the SIM_I2C driver replaces the whole `bk_i2c_*` API when enabled

Open items: run the probe with `CONFIG_SIM_I2C=y` (the bit-bang driver
replaces the HW controller and may handle this sensor's signaling),
verify the GC2145 DVDD strap (1.8 V variant per schematic), confirm the
module-to-connector seating.

## Not yet working — SD-NAND 1 GB (SDIO host, P14-P19)

Test app: `nand_fs_test` (raw block benchmark + FATFS over the chip).

Bring-up implemented:

- `CONFIG_SDIO_HOST + CONFIG_SDCARD + CONFIG_SDIO_V2P0 +
  CONFIG_SDIO_GDMA_EN` (the vendor `bk_sd_card_*` driver)
- power rail ensured first: NAND_VDD ← LDO_3V3, enabled by P52
  (LDO33_EN) — driven high before init
- init at 100 kHz (SDIO_HOST_CLK_100K), 1-line identification bus,
  BAKP_SDIO power vote handled internally by the driver
- external pull-ups present (CMD/DAT 10K, schematic sheet 4)
- retried for 20 s after power-up

Result: `CMD8 SEND_IF_COND` and `CMD55 APP_CMD` time out
(`BK_ERR_SDIO_HOST_CMD_RSP_TIMEOUT`) on every attempt — the chip has
not yet answered any card command. CMD0 has no response line and cannot
be checked.

Open items: try the identification at a lower/faster controller clock
(`CONFIG_SDIO_HOST_DEFAULT_CLOCK_FREQ`), try 4-line from the start
(`CONFIG_SDIO_4LINES_EN=y`), inspect the vendor `sdio_driver.c`
(non-host variant) used by `CONFIG_SDCARD`, and compare against the
genie USB-MSC path (`usbd_msc.c` calls the same `bk_sd_card_*` APIs).

## Investigation notes

- `bk_gpio_pull_down()` is a no-op on this SDK (writes pull_mode=0) —
  use `bk_gpio_set_config` with `GPIO_PULL_DOWN_EN`.
- The camera I2C and the g-sensor I2C may share the same pads
  (IIC1/IIC2 net naming vs. the actual GPIO mapping needs a definitive
  check against the schematic net list).
- The vendor projects (beken_genie etc.) enable the configs for these
  peripherals but **none of them exercises the g-sensor, camera, or
  SD-NAND at boot** (the camera only opens on demand; the gsensor/camera
  sleep config uses handles that require a prior CLI/demo init). So
  there is currently no vendor-verified boot log proving these three
  chips work out of the box — the bring-up on our side is the first
  attempt, and any missing vendor-side step (clock tree, PM votes, pin
  group table, boot order) is still a candidate cause.
- The full beken_genie firmware (3 cores) runs on the board and is the
  reference environment for further comparison.
