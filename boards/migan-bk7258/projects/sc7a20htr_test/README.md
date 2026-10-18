# sc7a20htr_test

Exercises the board's **SC7A20HTR** 3-axis MEMS accelerometer (g-sensor,
Silicon Labs-compatible / LIS2DH12 register layout, Silan).

> **STATUS: the sensor does not respond reliably on this board.** The vendor
> driver, the vendor CLI, my hardware-I2C path and a raw GPIO bit-bang all
> fail to read the chip reliably (details and evidence below). The software
> stack is complete and correct; the remaining suspicion is the sensor's
> hardware/strapping. No bench instruments were available to confirm.

## Hardware (migan schematic, sheet 4)

| signal | pin / resource | notes |
|--------|----------------|-------|
| SCL | P20 (I2C0_SCL) | 4.7 K pull-up (IIC1 net) |
| SDA | P21 (I2C0_SDA) | 4.7 K pull-up (IIC1 net) |
| INT1 | P48 | R44 4.7 K pull-up to G_VDD |
| I2C address | 0x18 (SDO strapped low) | WHO_AM_I (0x0F) = 0x11 |
| power | VDD pin 7 + VDDIO pin 3 from G_VDD | G_VDD <- R37 0R <- VDDGPIO (chip pin 51) |
| straps | SDO (pin 1), CS (pin 10) | CS must be HIGH for I2C mode |
| datasheet | `D:\board_database\main-migan-BK7258\sc7a20h_ds.pdf` | |

## Vendor stack (all ready-to-use, under `bk_idk/components/bk_gsensor/`)

| piece | file | gated by |
|-------|------|----------|
| SC7A20 driver | `gsensor_sc7a20.c` | `CONFIG_GSENSOR_SC7A20_ENABLE` |
| Demo (task+queue) | `gsensor_demo.c` | `CONFIG_GSENSOR_DEMO_EN` |
| Shake algorithm | `arithmetic_module.c` | `CONFIG_GSENSOR_ARITHEMTIC_DEMO_EN` |
| CLI commands | `bk_cli/cli_gsensor.c` | `CONFIG_GSENSOR_TEST_EN` |
| API | `include/components/bk_gsensor.h` | `CONFIG_GSENSOR_ENABLE` |
| docs | `docs/.../peripheral/bk_gsensor.rst` | |

Driver details: I2C0 at 100 kHz, 7-bit addr 0x18, WHO_AM_I 0x11 check,
CTRL_REG1 ODR/axes, FIFO reads, INT1 (P48) ISR + 450 ms poll timer.

Vendor usage: `bk_init.c` auto-calls `gsensor_demo_init()` when
`GSENSOR_ENABLE && GSENSOR_DEMO_EN`. `gsensor_demo_open()` sends the INIT
opcode which runs `bk_gsensor_init("sc7a20")` + configuration. The only
vendor runtime consumer is `gsensor_enter_sleep_config()` on deep-sleep
entry ("shake in an S-shape to wake"), and it uses a handle that is only
set by that INIT opcode - **no vendor project initializes the sensor at
boot**, so there is no vendor proof it ever worked on this board.

Console test (enabled in this project):
```
gsensor init
gsensor open
```
Success prints `SL_Read_Reg = 17` (0x11); failure prints
`read chip id fail`.

## What this project's app does

1. Brings up I2C0 and **raises the SCL-timeout threshold to 0xFFFF**.
   `i2c_hal_configure()` (inside every `bk_i2c_init`) hardcodes the SCL
   timeout threshold to **4** and enables it; the SC7A20H clock-stretches
   SCL by design (datasheet 10.1), so every transfer aborted with
   "SCL timeout triggered, restart i2c". The disarm must be applied
   *after* `bk_i2c_init` because the configure call re-arms it.
2. Soft-resets the sensor (0x68 = 0xA5) and runs the reference init from
   `mydazy/esp_sc7a20h`: CTRL1=0, CTRL4=0x80 (high-res, 2 g), CTRL2=0x01,
   CTRL5=0x08, CTRL3=0x40, CTRL6=0x02, CTRL1=0x47 (50 Hz + XYZ).
3. Checks WHO_AM_I; on success streams X/Y/Z in mg (12-bit -> >>4,
   1 mg/LSB) every 500 ms, LED1 (P40) = motion, LED2 (P41) = heartbeat.
   On failure it prints a hint to use the vendor CLI and idles.

A full **bit-bang I2C driver** (GPIO, 100 kHz, 9-clock bus recovery) is
also in the file (`bh_stream()`); it is currently not invoked by the app
(kept for diagnosis - it is the only path that ever read the real
WHO_AM_I, once).

## Investigation log (evidence)

| experiment | result |
|------------|--------|
| HW I2C `bk_i2c_memory_read` (1 st approach) | SCL-timeout abort, `read chip id fail` |
| Address sweep 0x08..0x77 | *all* addresses "ACK" with data 0x00 (phantom: the driver does not gate on NACK, and the bus reads low during transfers) |
| Bus idle levels (pads as inputs) | SCL=1, SDA=1 - healthy |
| External pull-up check (internal pulls killed) | both stay high - external 4.7 K confirmed |
| G_VDD detector (P48 + true internal pull-down via `bk_gpio_set_config`) | P48 = 1 -> **G_VDD alive** (an earlier test wrongly said dead: `bk_gpio_pull_down()` writes pull_mode=0 = pull disabled - do not use it) |
| Cold power cycle (unplug USB) | no change |
| Vendor CLI `gsensor open` | `SCL timeout triggered` -> `read chip id fail` (identical failure) |
| SCL timeout disarmed | transaction completes, WHO reads 0x00 |
| SCL timeout raised to 0xFFFF | same, WHO reads 0x00 |
| Raw GPIO bit-bang @100 kHz | NACKs (0xFF) - except **one run** that read `WHO_AM_I = 0x11` at 0x18 |
| Datasheet register map | WHO_AM_I 0x11, 12-bit left-aligned, 0x68=0xA5 soft reset, 0x6F I2C disable, 0x57 DIG_CTRL SDO_PU |

Conclusion: the I2C controller/HAL quirks are understood and worked
around (SCL timeout), but the sensor itself answers only sporadically -
once correctly (WHO_AM_I 0x11 at 0x18, so the wiring/address are right)
and otherwise not at all. With power (G_VDD), pull-ups and wiring
verified in software, the open suspicion is the sensor's own connection
or strapping:

- **CS (pin 10) must be high for I2C mode** (datasheet 7.2). The
  schematic shows R41 = NC near CS; the chip has an internal pull-up to
  VDDIO. If CS floats low, the part runs in SPI mode and produces exactly
  this "sporadic/no I2C" behaviour. Measure CS - expect ~3.3 V.
- **SDO (pin 1)**: 0 V = address 0x18 (matches). Datasheet also warns the
  internal SDO pull-up must be disabled (0x57 SDO_PU) when SDO is
  grounded.
- VDD pin 7 / VDDIO pin 3: expect ~3.3 V.

If those are correct, the sensor module itself (LGA solder joints or a
counterfeit part) is the remaining suspect. The ESP32 reference project
`example-mydazy__esp_sc7a20h-v2.0.0-basic_motion_wakeup` can discriminate:
wire the sensor to an ESP32 and run it.

## Build / flash

```
cd D:/armino/beken-prj
make build BOARD=migan-bk7258 PROJECT=sc7a20htr_test

bk_loader.exe download -p 42 -b 1500000 ^
    -i D:/armino/beken-prj/build/migan-bk7258/sc7a20htr_test/bk7258/all-app.bin -s 0
```

Only the cp0 image is needed; cp1/cp2 idle.

## Config

```
CONFIG_I2C=y
CONFIG_I2C_PM_CB_SUPPORT=y          # I2C0 is in the BAKP power domain
CONFIG_GSENSOR_ENABLE=y
CONFIG_GSENSOR_SC7A20_ENABLE=y
CONFIG_GSENSOR_DEMO_EN=y            # auto demo thread from bk_init
CONFIG_GSENSOR_ARITHEMTIC_DEMO_EN=y
CONFIG_GSENSOR_TEST_EN=y            # "gsensor ..." CLI
```
