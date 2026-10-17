# dual_lcd_test

Drives the two GC9D01 SPI LCD modules of the migan-bk7258 board **from cp0
via GPIO bit-bang** - independent of the QSPI hardware, the vendor LCD
driver stack and cp1.

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
- Bit-bang SPI writing the AON GPIO cfg registers directly
  (`gpio_ll_set_output_value`, then precomputed full-cfg-word single
  stores). Both panels are clocked in lockstep.
- Speed: ~1.2 s per full frame push (409,600 bits ≈ 350 kbit/s; the AON
  register bus caps at ~475 ns per access). Fine for text/patterns.
- GC9D01 init sequence taken verbatim from the vendor driver
  (`bk_peripheral/src/lcd/spi/lcd_spi_gc9d01.c`).
- Pages are laid out for the circular active area: ring border +
  centered text only. Banner shows `BK7258 / SW / LCD1|LCD2` (SW =
  software bit-bang); the info page adds pass counter and RGBW chips.
- Test patterns (both panels mirrored): checkerboard, R/G/B/W fills,
  color bars, gradient.

## Build / flash

```
cd D:/armino/beken-prj
make build BOARD=migan-bk7258 PROJECT=dual_lcd_test

D:/board_database/main-migan-BK7258/BEKEN_BKFIL_V2.1.12.2_20251030/bk_loader.exe ^
    download -p 42 -b 1500000 ^
    -i D:/armino/beken-prj/build/migan-bk7258/dual_lcd_test/bk7258/all-app.bin -s 0
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

To make the hardware path work: enable `CONFIG_QSPI=y` (or call
`bk_qspi_driver_init()` before `lcd_spi_init()`), then the vendor
`lcd_spi_display_frame()` flow should drive both panels at
LCD_QSPI_60M (480 MHz PLL / 7). The bit-bang driver in this project
stays as the cp0-only fallback.
