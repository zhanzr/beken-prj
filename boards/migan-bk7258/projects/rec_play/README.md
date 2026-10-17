# rec_play

Raw 10 s voice recorder / player for the migan-bk7258 - the button-driven
port of `esp32-demo/p4-cb-v3_2/app/touch_rec_play` (its touch UI replaced
by the board keys).

- 16 kHz mono 16-bit PCM through the on-chip codec (no external codec)
- Record to the SPI NOR (raw region at 0x610000), playback from it
- cp0-only; GDMA repeat rings + half/finish ISRs (the aud_tras_drv
  pattern), no media service / cp1 involved

## Behavior

| step | action |
|------|--------|
| boot | erase the record region in the background (~4 s); `rec: region ready` when done |
| **S1** | start a fixed 10 s recording; LED1 on, `rec: NN%` progress on the log |
| done | `rec: done`; Play is now enabled (S2) |
| **S2** | play the recording; LED2 + speaker PA on, `play: NN%` |
| done | back to idle; the recording is kept - S2 replays |
| **S3** | status print |

Keys are ignored while recording/playing.

## Hardware (migan schematic)

| function | pin / resource |
|----------|----------------|
| mic | on-chip ADC, MIC1 differential (MICP1/MICN1 + MICBIAS) |
| speaker | on-chip DAC (AUDLP/AUDLN) -> HT6873 PA; **PA enable (MUTE) = P50**, high = on |
| S1 / S2 / S3 | GPIO_13 / GPIO_12 / GPIO_8, active low (pull-ups) |
| LED1 / LED2 | P40 (recording) / P41 (playing) |
| storage | SPI NOR raw region 0x610000..0x64EFFF (316 KB, 10 s) |

## Build / flash

```
cd D:/armino/beken-prj
make build BOARD=migan-bk7258 PROJECT=rec_play

bk_loader.exe download -p 42 -b 1500000 ^
    -i D:/armino/beken-prj/build/migan-bk7258/rec_play/bk7258/all-app.bin -s 0
```

Only the cp0 image (`all-app.bin` @ 0x0) is needed; cp1/cp2 idle.

## Notes

- The PA enable is **P50** (HT6873 CTRL) - beken_genie's cp1 board config
  also owns P50 as a pulled-down output (muted default). P49 is the
  camera's DVP_PWR_CTRL, not audio.
- If the recording is silent/too quiet, adjust `adc.adc_gain` (default
  0x2d) and check MICBIAS is enabled by the codec in DIFFEN mode.
- The key-to-button mapping (S1=GPIO_13, S2=GPIO_12, S3=GPIO_8) follows
  `components/key/key_adapter.h`; if the buttons are swapped on the
  board, swap the defines at the top of `rec_play_main.c`.
