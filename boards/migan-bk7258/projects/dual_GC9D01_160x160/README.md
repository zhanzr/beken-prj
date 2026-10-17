# dual_GC9D01_160x160

Drives the two GC9D01 SPI LCD modules of the migan-bk7258 board from cp0,
**alternating the driving method each pass: SW (GPIO bit-bang) => HW
(QSPI hardware) => SW => HW ...**. The banner/info pages show the current
method (big "SW"/"HW") and the panel id ("LCD1"/"LCD2" on its own module).

Panels: 2x Galaxy Core **GC9D01**, 1.28" **round**, 160x160, RGB565 over
3-wire-style SPI (D/CX bit + 8 bit command / 16 bit pixels, SPI mode 0).

## Hardware connections (migan schematic)

| Signal | LCD1  | LCD2  |
|--------|-------|-------|
| SCLK   | P2 (GPIO_2)  | P22 (GPIO_22) |
| CS     | P3 (GPIO_3)  | P23 (GPIO_23) |
| MOSI   | P4 (GPIO_4)  | P24 (GPIO_24) |
| D/CX   | P5 (GPIO_5)  | P7  (GPIO_7)  |
| RESET  | P45 (GPIO_45)| P6  (GPIO_6)  |

Notes:

- LCD1 sits on the QSPI1 controller pads, LCD2 on the QSPI0 pads. This
  test does **not** use the QSPI controllers; the pins are driven as
  plain AON GPIO outputs.
- Both panels share the power rails: **P52 (GPIO_52) = LDO33_EN** must be
  driven high, it powers the panel logic and the backlight boost rail.
- Backlight: **P25 (GPIO_25) = PWM5**, 1 kHz at 30% duty (the boost
  driver needs PWM, a static high is not enough).

## Software approach

- cp0-only (`CONFIG_SYS_CPU0` guard; cp1/cp2 idle).
- **SW method**: bit-bang SPI writing the AON GPIO cfg registers
  directly (precomputed full-cfg-word single stores). ~1.0 s per
  full-frame push (~350 kbit/s; the AON register bus caps at ~475 ns
  per access). Both panels clocked in lockstep.
- **HW method**: the two QSPI units in 3-wire mode (CLK/CSN/IO0 only,
  480 MHz PLL / 7 ~= 60 MHz, the vendor LCD_QSPI_60M setting). Frames
  go as 4-byte `cmd_c` ops (the vendor "direct mode" byte packing:
  byte0 of the word hits the wire first). ~25 ms per frame per panel
  (~40x faster than SW). D/CX stays a plain GPIO - the same trick the
  vendor driver uses.
- Method switch = per-pad cfg word with/without the second-function
  bit; the QSPI dev-map (set by `bk_qspi_init`) persists.
- HW needs `CONFIG_QSPI=y` and `CONFIG_QSPI_LINE_MODE=1` in the cp0
  config (`config/bk7258/config`): without the former the QSPI driver
  framework never inits and every transfer times out; line mode 1 maps
  only CSN/CLK/IO0 so GPIO_25 (QSPI0 IO1) stays with PWM5 backlight.
- Bounded waits: a dead QSPI unit falls back to SW automatically
  (32 timeouts disable HW mode).
- GC9D01 init sequence taken verbatim from the vendor driver
  (`bk_peripheral/src/lcd/spi/lcd_spi_gc9d01.c`), sent once via SW.
- Pages are laid out for the circular active area: ring border +
  centered text only.
- Test patterns (both panels mirrored): checkerboard, R/G/B/W fills,
  color bars, gradient.

## Build / flash

```
cd D:/armino/beken-prj
make build BOARD=migan-bk7258 PROJECT=dual_GC9D01_160x160

D:/board_database/main-migan-BK7258/BEKEN_BKFIL_V2.1.12.2_20251030/bk_loader.exe ^
    download -p 42 -b 1500000 ^
    -i D:/armino/beken-prj/build/migan-bk7258/dual_GC9D01_160x160/bk7258/all-app.bin -s 0
```

Only the cp0 image (`all-app.bin` @ 0x0) needs flashing; cp1/cp2 stay
parked. The loader auto-resets the board via DTR/RTS (`do_reset_signal`)
before every attempt.

## Hardware driving path - investigation result

The panels sit on the two general QSPI units (pad map from
`middleware/soc/bk7258/hal/qspi_ll.h`):

| unit | CLK | CSN | IO0 | IO1 | IO2 | IO3 |
|------|-----|-----|-----|-----|-----|-----|
| QSPI0 | GPIO_22 | GPIO_23 | GPIO_24 | GPIO_25 | GPIO_26 | GPIO_27 |
| QSPI1 | GPIO_2  | GPIO_3  | GPIO_4  | GPIO_5  | GPIO_6  | GPIO_7  |

So LCD1 = QSPI1 (SCLK/CS/MOSI on CLK/CSN/IO0), LCD2 = QSPI0. The D/CX
lines are NOT carried by the QSPI unit: the vendor driver toggles them as
plain GPIOs around every command/data transfer
(`bk_gpio_set_output_low/high(LCDx_SPI_DC_PIN)` in
`lcd_spi_driver.c`). That is why LCD2's D/CX can sit on P7 (GPIO_7,
physically a QSPI1 IO3 pad) while its SPI rides QSPI0.

The reason the vendor QSPI path stalled in this project (`qspi_ll_wait_
cmd_done timeout`): `bk_qspi_init()` begins with
`QSPI_RETURN_ON_NOT_INIT()` - the QSPI driver framework must be
initialized first. That happens in `driver.c` only when
**`CONFIG_QSPI=y`** (`bk_qspi_driver_init()`). beken_genie enables it;
this project did not. The vendor LCD driver swallows the resulting error
(`BK_LOG_ON_ERR`), then pokes an unclocked, unpowered unit. Inside
`bk_qspi_init()` there is also the required power-domain vote
(`bk_pm_module_vote_power_ctrl(PM_POWER_SUB_MODULE_NAME_AHBP_QSPI[1],
PM_POWER_MODULE_STATE_ON)`), which runs only after the guard passes.

This project implements exactly that (see "Software approach"): the HW
method drives both panels through the QSPI units at ~25 ms/frame; the
SW bit-bang stays as fallback and is what sends the one-time GC9D01
init sequence.
