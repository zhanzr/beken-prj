# sc7a20htr_test

Exercises the board's **SC7A20HTR** 3-axis MEMS accelerometer (g-sensor)
through the vendor `bk_gsensor` component
(`components/bk_gsensor/gsensor_sc7a20.c`).

## Hardware (migan schematic, sheet 4)

| signal | pin |
|--------|-----|
| I2C SCL | P20 (I2C0_SCL) |
| I2C SDA | P21 (I2C0_SDA) |
| INT1 | P48 (falling edge) |
| I2C address | 0x18 (7-bit), chip ID reg 0x0F = 0x11 |
| power | G_VDD rail (always on) |

Pull-ups: IIC1_SCL/SDA have 4.7K on-board.

## What it does

- `bk_gsensor_init("sc7a20")` + open, 50 Hz ODR, ±2 G range, normal mode
- The vendor driver owns INT1 (P48) + a 450 ms poll timer and delivers
  XYZ batches to a registered callback
- This app prints the latest sample in **mg** every second; LED1 (P40)
  lights on motion (|mg| beyond the 1 g rest offset), LED2 (P41) is the
  1 Hz heartbeat

## Sample output

```
gsensor: x=   -12 mg y=    34 mg z=  1002 mg samples=183
gsensor: x=  -520 mg y=   180 mg z=   830 mg samples=213 MOVE
```

(1 g reads ~1000 mg on Z at rest; ±2 G range, 16 mg/LSB.)

## Build / flash

```
cd D:/armino/beken-prj
make build BOARD=migan-bk7258 PROJECT=sc7a20htr_test

bk_loader.exe download -p 42 -b 1500000 ^
    -i D:/armino/beken-prj/build/migan-bk7258/sc7a20htr_test/bk7258/all-app.bin -s 0
```

Only the cp0 image is needed; cp1/cp2 idle.

## Notes

- If init fails with `read chip id fail`, the SDO strap may select
  0x19 instead of 0x18 (`SC7A20_IIC_7BITS_ADDR` in the vendor driver).
