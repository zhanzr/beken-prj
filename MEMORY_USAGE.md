# Memory Usage — BK7258 (beken_genie / R1 board)

This document describes the memory available on the BK7258-based R1 board, how the
`beken_genie` firmware uses it, what actually ends up in the final `all-app.bin`, and
which parts of the image are closed-source prebuilt binaries.

All numbers are taken from this repository's actual build artifacts
(`build/beken_genie/*/app.map` linker memory configurations, `arm-none-eabi-size`,
the flash partition table, and the linked library lists) unless stated otherwise.

The BK7258 is a triple-core Cortex-M33 SoC. In this project:

| Core | Firmware binary | Role |
|------|-----------------|------|
| CPU0 | `app.bin`       | Main application: WiFi + BLE, audio/video engines, GUI, OTA, CLI |
| CPU1 | `app1.bin`      | Secondary application core (multimedia/AI offload) |
| CPU2 | `app2.bin`      | Small slave core (runs from PSRAM) |

---

## 1. Non-volatile memory

### 1.1 SPI NOR flash — 8 MiB used (16 MiB class chip)

External NOR flash, memory-mapped for XIP (execute-in-place) through a virtual
address window. The partition table (`projects/beken_genie/config/bk7258/partitions.csv`)
covers `0x000000 – 0x800000` (8 MiB); the build tooling passes 16 MB as the physical
flash size argument, so partitions only use the first half of the chip.

| Partition | Offset | Size | Used by | Content |
|-----------|--------|------|---------|---------|
| `primary_bootloader` | 0x00000 | 68 K | CPU0 boot | 2nd-stage bootloader (prebuilt, packed with partition table) |
| `primary_cpu0_app` | 0x11000 | 2856 K | CPU0 | Main application (`app.bin`, XIP) |
| `primary_cpu1_app` | 0x2db000 | 1496 K | CPU1 | Secondary application (`app1.bin`, XIP) |
| `primary_cpu2_app` | 0x451000 | 476 K | CPU2 | Slave application (`app2.bin`, XIP) |
| `ota` | 0x4c8000 | 3196 K | OTA | Downloaded update image staging area |
| `usr_config` | 0x7e7000 | 64 K | apps | User key/value configuration |
| `easyflash` | 0x7fa000 | 16 K | EasyFlash | KV parameter store |
| `sys_rf` | 0x7fe000 | 4 K | RF calibration | RF calibration data |
| `sys_net` | 0x7ff000 | 4 K | network | Network-calibration data |

### 1.2 Final `all-app.bin` composition

`all-app.bin` (4,962,810 bytes in the current build) is produced by the post-build
pack step (`middleware/boards/bk7258/bk7258_nosecurity.wrapper`) and contains,
concatenated at their partition offsets:

```
bootloader.bin (44,096 B)  +  app.bin (2,256,252 B)  +  app1.bin (789,784 B)  +  app2.bin (411,008 B)
```

packed per `configuration.json` and post-processed by the Beken
`cmake_Gen_image` / `cmake_encrypt_crc` tools (which add the BK image header
structure and per-block CRC, an ≈ 6.25 % size overhead). The `ota` partition is
**not** part of `all-app.bin`; OTA images are produced separately (`app_pack.rbl`).

### 1.3 SD NAND — 60 MB (board spec)

The R1 board carries a 60 MB SD NAND used as bulk storage (files, assets). It is
not part of the firmware image; the SDK supports littlefs/FATFS on it
(`mklittlefs` images can be appended by the pack flow when enabled).

### 1.4 OTP / eFuse

On-chip one-time-programmable memory (secure-boot flags, flash-encryption key
slots, chip UID). The pack flow can emit an `otp_efuse_config.json` programming
file; secure boot / flash encryption are **disabled** in this project
(`CONFIG_TFM=n`, no-security pack path).

---

## 2. Volatile memory

### 2.1 On-chip SRAM (~1.2 MiB total, statically partitioned at link time)

Measured from the linker memory configurations of the three built images:

**Code SRAM (IRAM, 0x08000000 – 0x08090000, 576 KiB)** — hot code placed in RAM
(`.iram` sections: ISRs, flash-bypass routines, WiFi/BT timing-critical code):

| Region | Address | Size | Core |
|--------|---------|------|------|
| IRAM cp0 | 0x08000000 | 244 KiB | CPU0 |
| IRAM cp1 | 0x08058000 | 224 KiB | CPU1 |
| (cp2 has no IRAM) | | | CPU2 |

**Data SRAM (0x28000000 – 0x280A0000, 640 KiB)**:

| Region | Address | Size | Core |
|--------|---------|------|------|
| RAM cp0 | 0x28000000 | 240.5 KiB | CPU0 data/bss/heap |
| SWAP (shared scratch) | 0x2803C800 | 2 KiB | inter-core |
| SRAM_BT | 0x2803D000 | 108 KiB | reserved for BT/media (`CONFIG_BT_REUSE_MEDIA_MEMORY`) |
| RAM cp1 | 0x28058000 | 224 KiB | CPU1 data/bss/heap |
| RAM cp2 | 0x28090000 | 64 KiB | CPU2 data/bss/heap |

**TCM, per core (3 cores × 32 KiB = 96 KiB)**: each core has its own
ITCM (16 KiB @ 0x00000000, critical code) and DTCM (~16 KiB @ 0x20000000,
latency-critical data).

### 2.2 External PSRAM — 16 MiB (window 0x60000000–0x64000000)

Large-capacity volatile memory behind the QSPI controller (auto-detected at boot;
the layout proves a 16 MiB chip: cp2's region ends exactly at 0x61000000).

Usage in this project (`CONFIG_PSRAM=y`, `CONFIG_PSRAM_AS_SYS_MEMORY=y`):

| Usage | Address | Size |
|-------|---------|------|
| CPU0 system heap | 0x60B00000 | 2 MiB (`CONFIG_PSRAM_HEAP_SIZE`) |
| Display memory slab | — | 5.7 MB (`CONFIG_PSRAM_MEM_SLAB_DISPLAY_SIZE`) |
| Video-encode slab | — | 1.4 MB |
| Audio / user slabs | — | 100 KB each |
| CPU2 code + data | 0x60F80000 | 512 KiB (the whole cp2 image runs here) |
| Task stacks | — | `CONFIG_TASK_STACK_IN_PSRAM=y` |

The display slab alone (frame buffers for the dual LCDs) plus the encode slab
explain most of the PSRAM budget; the rest is general heap for media buffers.

### 2.3 Actual usage of this build (from `arm-none-eabi-size`)

| Core | text (flash) | data (RAM init) | bss (RAM zero-init) |
|------|-------------|-----------------|---------------------|
| CPU0 | 2,230,396 B | 25,856 B | 148,136 B |
| CPU1 | 779,364 B | 10,420 B | 166,528 B |
| CPU2 | 400,592 B | 10,416 B | 71,704 B |

(`text` lands in the flash XIP regions plus IRAM/ITCM copies; `data`+`bss` consume
the data-SRAM partitions above, the remainder of each partition becomes the
core's SRAM heap. Additional code/data sections are placed in PSRAM by the
linker and are not fully reflected in these columns.)

---

## 3. What ends up in the final image — source vs. blackbox

The build system compiles a large part of the SDK from source, but **not
everything**: several components are linked as prebuilt static libraries
(`.a`) without source. Classification below is derived from the linked-library
lists in `build/beken_genie/*/app.map` and the SDK directory layout.

### 3.1 Built from source (readable, in-tree)

- Application code: `projects/beken_genie/main`, `components/app`, CLI, OTA, keys,
  power management, boarding service, etc.
- Kernel & platform: FreeRTOS kernel + ports (`components/os_source`), CMSIS,
  SoC drivers (`middleware/driver`), HAL, startup code, in-tree `components/bk_phy`
  adaptation layer, `components/bk_wifi` (supplicant/adapter layer)
- Network: `lwip_intf_v2_1` (the lwIP OS/port glue), netif, HTTP(S) client,
  WebSocket, MQTT (`ali_mqtt`), smart-config, airkiss
- Security: mbedtls (+ PSA/Dubhe crypto wrappers, `libsecurity.a` built in-tree;
  TLS runtime — TFM itself is disabled)
- Storage: littlefs, FATFS, EasyFlash, VFS, FlashDB
- GUI/media framework glue: LVGL (source), multimedia framework, audio/video
  engines, `audio_engine`, `video_engine` (project level)
- Bootloader: source exists under `properties/modules/bootloader/aboot`, but note
  the packed image actually uses the **prebuilt** `normal_bootloader` binary from
  `components/bk_libs/bk7258/bootloader/` (the pack flow inserts the partition
  table into it).

### 3.2 Closed-source prebuilt binaries (the "blackboxes")

**IDK core (`bk_avdk/bk_idk/components/bk_libs/<soc>/libs/`)** — linked into CPU0:

| Library | Function |
|---------|----------|
| `libwifi.a` | WiFi MAC/stack (the low-level WiFi core) |
| `libbk_phy.a`, `libcom_phy.a`, `libate_phy.a`, `libbk_phy_info.a` | Radio PHY + calibration |
| `libbk_coex.a` | WiFi/BT coexistence |
| `libbluetooth_controller_*.a` | BT/BLE controller (link layer) |
| `libbluetooth_host_*.a`, `libble_wifi_exchange.a` | BT host stack variants (DM/BLE/PAN) |
| `libbk_lwip.a` | lwIP stack core (only the OS/port glue is source) |
| `libuid.a`, `libwifi_csi.a` | UID service, WiFi CSI |

Into CPU1/CPU2 additionally: `libbk_phy.a`, `libusb.a` (USB device stack).

**AVDK (`bk_avdk/components/avdk_libs/bk7258/libs/`)** — multimedia, linked into CPU0:
`libaacdec`, `libfdk_aac_enc`, `libopus`, `libflac`, `libhelix` (MP3), `libsbc`,
`libg711`, `libg722`, `libadpcm`, `libwav`, `libogg` (codecs);
`libaec`, `libaec_v2/v3`, `libaud_ns`, `libagc`, `libaud_eq`, `libaud_vad`
(audio processing/AEC/NS/VAD); `libavi`, `liblibjpeg`, `libtjpg_sram/dtcm`,
`libscale_sram/dtcm`, `libvideo_osi`, `libaudio_osi` (image/video/audio plumbing);
`libfreetype`, `liblcd_font`, `libqrcodegen`, `libbk_server`.

**Third-party components (`bk_avdk/components/bk_thirdparty/`)** — shipped as
binaries; in **this** project only the Agora SDK is linked (it is the voice/AI
transport):

| Component | Status in `beken_genie` |
|-----------|------------------------|
| `agora-iot-sdk` (libs for bk7258) | **linked** (thousands of symbols in `app.map`) |
| `asr/wanson`, `asr/wanson_foreign`, `asr/wenwen`, `unisound` (offline ASR) | present in tree, **not linked** (alternate wake-word/ASR engines for other projects) |
| `cs2p2p`, `aac_decoder` (Andes `nds32` variants for other SoCs) | present, **not linked** |
| `LingXin_AI_IoT_VoiceChatEngine`, `aliyun_sdk` | present, **not linked** |
| `VolcEngineRTCLite` | present, **not linked** in this configuration |

**On-chip, never in any binary**: the mask boot ROM (first-stage boot) and the
hardware crypto engine firmware (DUBHE) are silicon-resident and always
closed-source.

### 3.3 Practical implications

- RF/WiFi/BT behavior, the lwIP core, and the media codecs cannot be rebuilt or
  patched from source; they are fixed `.a` files validated per SDK release
  (`components/bk_libs/<soc>/hash/libs_hash.txt` pins their hash and the
  toolchain version used to build them).
- The prebuilt libs were built with GCC 10.3 and linked here with GCC 15.3 —
  C-level ABI is compatible for this build, but a toolchain/hash mismatch makes
  the build stop by design (`bk_libs or toolchain version verify failed`).
- Everything application-level (business logic, GUI, network glue, audio/video
  engine wiring) is source-built and inspectable in the `app.map`.

---

*Generated from the local build of `ai_release/v2.0.1` (`make bk7258
PROJECT=beken_genie`). Numbers change with sdkconfig; regenerate via
`arm-none-eabi-size build/beken_genie/*/app.elf` and the `Memory Configuration`
section of each `app.map`.*
