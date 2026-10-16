# BK7258 Benchmarks (migan-bk7258) - measured results

Measured 2026-10-16. Firmware: `dhry_3c` / `coremark_3c` (Dhrystone 2.1,
2,000,000 runs; CoreMark 1.0.1, 1800 iterations). GCC arm-none-eabi 15.3.1,
flags `-Ofast -funroll-all-loops -fgcse-sm -finline-limit=500 -fno-schedule-insns`.

## Results (cores run one-by-one)

| Core | Config freq | Dhrystone/s | DMIPS/MHz (claimed) | CoreMark/s |
|------|-------------|-------------|---------------------|------------|
| CPU0 | 240 MHz | **604,595** | 1.434 | **648.88** |
| CPU1 | "480 MHz" (unverified) | **124,843** | 0.148 (vs claimed) | **~143** (timing-derived) |
| CPU2 | - | does not run (blocked) | - | - |

## Analysis

1. **CPU0** is fully working: XIP execution with caches, both benchmarks
   validated ("Correct operation validated" for CoreMark).
2. **CPU1** boots (power-vote not required; released directly at its XIP link
   address 0x021b0000) and runs the full benchmark with correct validation.
   Measured throughput is **4.84x lower than CPU0**. Contributors visible in
   the logs: cp1's flash-driver client fails to connect
   (`flash-drv-client connect failed -8`) and cp1's PSRAM autodetect fails -
   cp1 fetches code/data through a slow, un-optimized path. The "480 MHz"
   frequency is the configured value, not independently verified; the measured
   0.148 DMIPS/MHz (at the claimed clock) reflects the memory-path penalty.
3. **CPU2** does not execute its main() despite: release at its exact link
   address (0x02220000, verified from its vector table and linker map),
   power-domain vote ON, and the raw image verified present at its partition
   offset (0x451000, readback-verified). cp2 executes from PSRAM; its boot
   requires the closed pm library's copy/XIP setup, whose cp1-boot sibling
   flow is also broken (boot address 0x0). Needs Beken support or the pm
   library source.

## Multi-core mechanics that DO work

- All three images build from one project (per-core `CONFIG_SYS_CPUn` guards).
- cp0 releases cp1 via `reset_cpu1_core(0x021b0000, 1)` - cp1 boots and runs.
- cp0<->cp1 UART logging works (AMP asynclog, `cpu1:`-prefixed lines).
- The cp0->cp1 mailbox GO has a delivery issue (channel direction semantics,
  cp1 runs via its timeout fallback - by design).

## Operational notes

- Reflashing does not reset cp1/cp2: a running slave core keeps executing
  stale code from its caches. RST (or a cp0-side reset) clears it.
- The pm library's cp1-boot vote, when cast, spams UART (~600 lines/s) and
  blocks re-flashing ("GetBus fail"); RST + quick re-flash recovers.
- `CONFIG_PM_SUPER_DEEP_SLEEP` (default on) darkens an idle board; RST revives.
