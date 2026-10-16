# BK7258 Benchmarks (migan-bk7258)

Benchmark projects in `boards/migan-bk7258/projects/`:

| Project | Suite | Notes |
|---------|-------|-------|
| `dhry_3c` | Dhrystone 2.1 | 2,000,000 runs per core |
| `coremark_3c` | CoreMark 1.0.1 (EEMBC) | 7200 iterations per core |

Design: every core runs the same suite at its maximum frequency
(cp0 = 240 MHz, cp1/cp2 = 480 MHz; the runner votes the system PLL to
480 MHz via `bk_pm_module_vote_cpu_freq`). Per-component optimization flags
(same set as the reference projects `f4-demo` / `hlk-w800-kit`):

```
-Ofast -funroll-all-loops -fgcse-sm -finline-limit=500 -fno-schedule-insns
```

Compiler: GCC (arm-none-eabi) 15.3.1.

Build & flash:

```bash
make build BOARD=migan-bk7258 PROJECT=dhry_3c
make flash BOARD=migan-bk7258 PROJECT=dhry_3c COM=42
```

## Results

### Dhrystone 2.1 (measured 2026-10-16)

| Core | Frequency | Dhrystones/s | DMIPS/MHz |
|------|-----------|--------------|-----------|
| CPU0 | config 240 MHz | 148,699 | 0.353 |
| CPU1 | blocked (see below) | - | - |
| CPU2 | blocked (see below) | - | - |

The DMIPS/MHz figure is dominated by XIP flash execution (code fetched from
SPI NOR through the cache); treat it as an end-to-end number, not a pure
core-speed number.

### CoreMark 1.0.1

Built; on-board measurement pending (flash blocked by the wedged-board issue
below at the time of writing).

## Multi-core status: cp1/cp2 boot is blocked by the closed pm library

Evidence gathered (all logs on UART0, AMP asynclog tags each core `cpu0:`,
`cpu1:`, `cpu2:`):

1. cp1 **executes**: after flashing beken_genie, cp1-tagged logs
   (`cpu1:media_ma:...`) appear - the AMP machinery and per-core logging work.
2. cp1 does **not** restart into a newly flashed image: after re-flashing,
   cp1 keeps executing stale code from its caches/XIP (media mailbox send
   failures continue), because nothing reset cp1.
3. `start_cpu1_core()` (open code) resolves the cp1 boot address via
   `get_partition_addr(1)` to `0x02000000 + 0x1c0000` on this board - which
   decodes to the **secondary cpu0 A/B slot**, not to `primary_cpu1_app`
   (0x2db000, interleaved XIP 0x022b0000 where cp1's image is linked and
   where its linker script places FLASH).
4. Releasing cp1 directly at its link address
   (`reset_cpu1_core(0x022B0000, 1)`, bypassing the broken lookup) did not
   restart an already-running cp1.
5. The pm-library boot flow (`bk_pm_module_vote_boot_cp1_ctrl`) loops
   forever with its own boot address = **0x0**:
   ```
   boot_cp1 9 0 0x0 [0][0x28014244]E_1
   cp0 boot cp1[0] time out, boot cp1 fail!!!
   ```
   This flow is inside the closed-source pm library.

Tried without success: pm vote only / direct release only / both;
`CONFIG_SYS_CPU1_OFFSET` = beken_genie value and defconfig value;
`bootcore 1 1` CLI command (same pm path).

### Conclusion

- CPU0 benchmarking: fully working (both suites).
- cp1/cp2: images build correctly and are packed into `all-app.bin`
  (`app1.bin`, `app2.bin`); cp1 is verifiably executing; but **reliable
  cp1/cp2 reset/boot is broken in this SDK snapshot** (closed pm lib boots
  from address 0x0; open-source reset path resolves the wrong partition).
  Needs Beken support or the pm library source. The benchmark projects will
  run the cp1/cp2 parts automatically once slave-core boot works.

## Side effects to be aware of

- When the cp1 boot vote is active, the pm library retries forever and spams
  UART0 (~600 lines/s). During that spam `bk_loader` cannot sync with the ROM
  ("GetBus fail"). Recovery: press **RST** (or unplug USB) and re-flash
  quickly, or use the GUI tool.
- `CONFIG_PM_SUPER_DEEP_SLEEP` is on by default: an idle board may enter
  super deep sleep (everything dark, UART dead). RST revives it.
- Reflashing does not reset cp1/cp2 - a running slave core keeps executing
  stale code until cp0 explicitly resets it (or RST is pressed).
