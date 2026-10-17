# BK7258 Benchmarks (migan-bk7258) - final measured results

Measured 2026-10-16. Firmware: `dhry_2c` / `coremark_2c` (Dhrystone 2.1,
2,000,000 runs; CoreMark 1.0.1, 7200 iterations). GCC arm-none-eabi 15.3.1,
flags `-Ofast -funroll-all-loops -fgcse-sm -finline-limit=500 -fno-schedule-insns`.
Cores run one-by-one; all numbers from UART capture, flash readback-verified.

## Results

| Core | Config freq | Dhrystone/s | DMIPS/MHz (vs claimed) | CoreMark/s |
|------|-------------|-------------|------------------------|------------|
| CPU0 | 240 MHz | **604,595** | 1.434 | **648.88** (validated) |
| CPU1 | "480 MHz" (unverified) | **124,843** | 0.148 (vs claimed) | **~141.5** (timing-derived) |
| CPU2 | - | does not run (blocked) | - | - |

## Analysis

1. **CPU0** fully working: XIP execution, both benchmarks validated
   ("Correct operation validated").
2. **CPU1** boots (released directly at its XIP link address 0x021b0000) and
   runs both benchmarks with correct validation. Measured throughput is
   **4.84x lower than CPU0**. Visible contributors: cp1's flash-driver client
   fails to connect (`flash-drv-client connect failed -8`), cp1's PSRAM
   autodetect fails - cp1 fetches through a slow, un-optimized path. The
   "480 MHz" is the configured value, not independently verified.
3. **CPU2** does not execute main() despite: release at its exact link
   address (0x02220000, verified from vector table + linker map), the power
   domain vote ON, and its raw image verified present at its partition offset
   (0x451000, readback-verified). cp2's boot depends on the closed pm
   library's copy/XIP setup, whose cp1-boot sibling flow is also broken
   (boot address 0x0). Needs Beken support or the pm library source.

## Multi-core mechanics that DO work

- All three images build from one project (per-core `CONFIG_SYS_CPUn` guards).
- cp0 releases cp1 via `reset_cpu1_core(<link addr>, 1)` - cp1 boots and runs.
- cp0<->cp1 UART logging works (AMP asynclog, `cpu1:`-prefixed lines).
- CoreMark/Dhrystone results are per-core tagged and correct (validation
  passed on both cp0 and cp1).

## Operational notes

- Reflashing does not reset cp1/cp2: a running slave core keeps executing
  stale code from its caches. RST (or a cp0-side reset) clears it.
- A cp1-boot vote, when cast, makes the pm library retry cp1 boot forever at
  a wrong address (0x021c0000) and spam UART (~600 lines/s), blocking
  re-flashing ("GetBus fail"); RST + quick re-flash recovers. The
  `bk_pm_module_check_cp1_shutdown()` auto-close is patched out in this repo
  (see the vendor-tree diff) so no cp1 boot vote is needed.
- `CONFIG_PM_SUPER_DEEP_SLEEP` (default on) darkens an idle board; RST
  revives it.

## dual_GC9D01_160x160 findings (2026-10-17, evening)

The dual-LCD driving architecture, fully traced:

- The two GC9D01 160x160 panels sit on the two QSPI-backed SPI interfaces:
  LCD_SPI_ID0 (QSPI0: SCLK=P22, CS=P23, MOSI=P24, DC=P7, RST=P6) and
  LCD_SPI_ID1 (QSPI1: SCLK=P2, CS=P3, MOSI=P4, DC=P5, RST=P45) - the
  driver's pin defaults match the migan schematic exactly.
- beken_genie compiles the GC9D01 driver into the **cp1** firmware only
  (CONFIG_LCD_SPI_GC9D01=1 in the cp1 sdkconfig; the cp0 build does not
  have it). cp0 opens the panels via media_app_lvgl_open(&lcd_open)
  which votes cp1 boot and forwards the open to cp1's media service.
- dual_GC9D01_160x160 currently runs the same driver on **cp0** (the cp0 build
  now registers the GC9D01 via the project Kconfig.projbuild). The
  backlight works (LDO33 rail P52 + P25=PWM5 @ 30%), but the panel
  SPI-over-QSPI writes stall (qspi_ll_wait_cmd_done timeout,
  lcd_spi_send_cmd ret=-0x2007) - the QSPI unit's cmd_start_done never
  completes, consistent with a missing QSPI clock/pad setup that
  beken_genie's cp1-side flow provides.

Next steps: either port the remaining cp1-side display init
(bk_peripheral_init + the QSPI clock/pad config) into the cp0-driven
test, or move the pattern push into dual_GC9D01_160x160's cp1 firmware
(the cp1 build already has the full GC9D01 driver). The vendor LCD
guide: docs.bekencorp.com bk_avdk bk7258 display/lcd_user_guide.
