# migan-bk7258 — board hardware status

Peripherals verified on the board. Three optional I2C/SDIO peripheral
chips are **electrically silent** despite exhaustive software bring-up
following the vendor examples. This document summarizes the evidence;
per-project details live in the project READMEs.

## Working

| peripheral | project | status |
|------------|---------|--------|
| Dual GC9D01 round LCDs (SW bit-bang + HW QSPI 60 MHz) | `dual_GC9D01_160x160` | working, ~25 ms/frame (HW) |
| Voice recorder (on-chip codec mic + speaker, 16 kHz) | `rec_play` | record + playback verified |
| WiFi station (scan, connect, DHCP) | `wifi_con_test` | connected, IP via DHCP |
| Web server (LEDs, DTS temp, VBAT) | `http_server` | live at board IP |
| Raw SD-NAND / FATFS test app | `nand_fs_test` | app runs; **chip silent** (see below) |

## Not working — g-sensor SC7A20HTR (I2C0, 0x18)

Details: `sc7a20htr_test/README.md`. Vendor driver + CLI implemented;
bus verified healthy (idle levels, external pull-ups, G_VDD alive);
the chip answered **once** (WHO_AM_I = 0x11, correct address) and has
been silent since, across cold power cycles. Suspected intermittent
connection (LGA solder) or dead part.

## Not working — camera GC2145 (DVP)

The vendor's complete camera bring-up (`bk_dvp_camera_enumerate()`:
power P49 → DVP GPIO init → MCLK enable → I2C → all 6 sensor detects,
58 s of probing) finds **no camera**. Bit-bang probes on both candidate
buses (P42/P43 and P20/P21) at 4 addresses: all NACK. Genie's own
`media_app_camera_open` (all 3 cores flashed) blocks forever without
reaching the camera driver. Camera power (P49 = DVP_PWR_CTRL → camera
LDOs) is driven by software.

## Not working — SD-NAND 1 GB (SDIO host, P14-P19)

The chip presents as an SD card; the vendor driver flow is fully
implemented in `nand_fs_test`:

- `CONFIG_SDIO_HOST + CONFIG_SDCARD + CONFIG_SDIO_V2P0 + CONFIG_SDIO_GDMA_EN`
- init at 100 kHz (SDIO_HOST_CLK_100K), 1-line bus, BAKP_SDIO power vote
  (handled internally by the driver)
- power rail ensured: NAND_VDD ← LDO_3V3, enabled via P52 (LDO33_EN)
- external pull-ups present (CMD/DAT 10K, schematic sheet 4)
- retried for 20 s after power-up

Result: `CMD8 SEND_IF_COND` and `CMD55 APP_CMD` both time out
(`BK_ERR_SDIO_HOST_CMD_RSP_TIMEOUT`, err -3902) on every attempt — the
chip never sends a response. CMD0 cannot be checked (no response line).

## Pattern

All three silent chips (g-sensor, camera, SD-NAND) are the board's
*optional* peripheral chips; every core chip (SPI flash, RF/WiFi, PMIC,
CH340, codec) works. The schematic lists camera/g-sensor as configurable
options (multiple DVDD variants, "GC2053/HM1055/GC2145"). Since software
bring-up has followed the vendor references exactly and the buses verify
healthy at the electrical level (idle levels, pull-ups, rails), the
remaining explanations are: the specific chips/modules are not populated
on this board revision, are incompatible variants, or are dead.

Hardware checks that would settle each case (meter/scope):
1. g-sensor: VDD pin 7 / VDDIO pin 3 voltage; I2C activity during probe
2. camera: VDD pin 7 (2.8 V), DVDD (1.8 V), MCLK presence at pin 13
3. SD-NAND: VDD pin 8 (3.3 V), CLK pin 3 toggling during init,
   CMD pin 5 activity
