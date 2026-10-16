# BK7258 Benchmarks (migan-bk7258) - final measured results

Measured 2026-10-16. Firmware: `dhry_3c` / `coremark_3c` (Dhrystone 2.1,
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
