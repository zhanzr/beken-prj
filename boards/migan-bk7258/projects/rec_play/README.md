# rec_play

Raw 10 s voice recorder / player for the migan-bk7258 - the button-driven
port of `esp32-demo/p4-cb-v3_2/app/touch_rec_play` (touch UI replaced by
the board keys, plus a GUI on the round GC9D01 panels).

- 16 kHz mono 16-bit PCM through the on-chip codec (no external codec)
- 10 s recording held in a 320 KB **PSRAM** buffer (`psram_malloc`) -
  no flash, nothing to erase, instant start
- cp0-only; GDMA repeat rings + finish ISR per 128 ms (the
  `aud_tras_drv` pattern); ADC clock = XTAL (the APLL default does not
  clock the ADC here)
- GUI on both round panels (GC9D01): REC / PLAY buttons + progress bar;
  one-time init via the SW bit-bang, per-frame pushes via the QSPI
  units at 480M/7 (~25 ms), from the dual_GC9D01_160x160 driver

## Behavior

| step | action |
|------|--------|
| boot | panel init, PSRAM buffer, UI: REC (red) + PLAY (dim until a recording exists) |
| **S1** | 10 s recording; LED1 on; red progress bar + `rec: NN%` |
| done | PLAY turns green |
| **S2** | playback; LED2 + speaker PA on; blue progress bar + `play: NN%` |
| done | back to idle; the recording is kept - S2 replays |
| **S3** | status print |

Keys are ignored while recording/playing. A dead ADC auto-aborts the
record after 3 x 2 s timeouts (no infinite RED-LED hang).

## Hardware (migan schematic)

| function | pin / resource |
|----------|----------------|
| mic | on-chip ADC, MIC1 differential (MICP1/MICN1 + MICBIAS) |
| speaker | on-chip DAC (AUDLP/AUDLN) -> HT6873 PA; **PA enable (MUTE) = P50**, high = on |
| S1 / S2 / S3 | GPIO_13 / GPIO_12 / GPIO_8, active low (pull-ups) |
| LED1 / LED2 | P40 (recording) / P41 (playing) |
| panels | 2x GC9D01 round 160x160 (see dual_GC9D01_160x160/README.md) |
| record buffer | PSRAM @ 0x60B00000 window, 320 KB |

## Build / flash

```
cd D:/armino/beken-prj
make build BOARD=migan-bk7258 PROJECT=rec_play

bk_loader.exe download -p 42 -b 1500000 ^
    -i D:/armino/beken-prj/build/migan-bk7258/rec_play/bk7258/all-app.bin -s 0
```

Only the cp0 image (`all-app.bin` @ 0x0) is needed; cp1/cp2 idle.

## Notes

- If the recording is silent/too quiet, adjust `adc.adc_gain` (default
  0x2d) and the analog mic gains; check MICBIAS is enabled by the codec
  in DIFFEN mode.
- Key mapping (S1=GPIO_13, S2=GPIO_12, S3=GPIO_8) follows
  `components/key/key_adapter.h`; swap the defines if the buttons
  differ on the board.
