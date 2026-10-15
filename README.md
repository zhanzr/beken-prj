# beken-prj

Custom firmware projects for Beken BK7258-based boards, kept separate from the
vendor SDK.

```
beken-prj/
├── vendor/bk_aidk/            # vendor SDK (git submodule of github.com:bekencorp/bk_aidk)
├── boards/
│   └── migan-bk7258/          # board: projects, photos, board config
│       ├── projects/
│       │   ├── blink_hello/   # our minimal demo (2 LEDs + UART0 hello)
│       │   └── beken_genie/   # vendor AI-solution project, customized for this board
│       └── board_images/
├── patches/                   # local patches applied on top of the vendor SDK
├── tools/                     # build/flash helpers
├── Makefile                   # entry point: make build BOARD=... PROJECT=...
└── MEMORY_USAGE.md            # memory map & open/closed source analysis
```

## Why patches?

The vendor SDK needs small fixes for this repository's Windows/MSYS2 build flow
(toolchain path, path-handling bugs, native Windows pack tools, GCC 15 warning
downgrades). They live in `patches/` and are applied **on top of the pristine
submodule** by `tools/apply-vendor-patches.sh`, so the vendor tree stays
reviewable and `git submodule update` remains meaningful.

## Setup (fresh clone)

```bash
git clone --recurse-submodules git@github.com:zhanzr/beken-prj.git
cd beken-prj
bash tools/apply-vendor-patches.sh        # after submodule checkout
cp config.local.mk.example config.local.mk   # set your COM port
```

Python requirements inside MSYS2 (once):
```bash
pacman -S --needed mingw-w64-x86_64-python-future mingw-w64-x86_64-python-pycryptodome \
                  mingw-w64-x86_64-python-yaml mingw-w64-x86_64-python-numpy \
                  mingw-w64-x86_64-python-cryptography
python3 -m pip install --user --break-system-packages 'setuptools<81' click_option_group pexpect
```

Toolchain: Arm GNU Toolchain (arm-none-eabi) on PATH; `CONFIG_TOOLCHAIN_PATH`
is patched to `D:/Arm/GNUTOO~1/bin` (adjust `patches/*.patch` for other machines).

## Build & flash

```bash
make build BOARD=migan-bk7258 PROJECT=blink_hello
make flash BOARD=migan-bk7258 PROJECT=blink_hello COM=42
```

- `beken_genie` is built from the **board overlay** `boards/migan-bk7258/projects/beken_genie`
- `blink_hello` is built from this repo
- outputs land in `build/<board>/<project>/<soc>/all-app.bin`

## Boards

| Board | SoC | Projects |
|-------|-----|----------|
| [migan-bk7258](boards/migan-bk7258) | BK7258 (3x Cortex-M33) | blink_hello, beken_genie |

## CLI flash (bk_loader)

```cmd
bk_loader.exe download -p <COM#> -b 1500000 -i all-app.bin -s 0
bk_loader.exe read -p <COM#> -b 1500000 -f dump.bin@0-0x255e00   # verify readback
```

## Windows image tools (no WSL needed)

Set in `boards/<board>/board.mk` or `config.local.mk`:

- `GEN_IMAGE_WIN_EXE`   - BEKEN_GEN win build (SDK's own exe segfaults)
- `ENCRYPT_CRC_WIN_EXE` - BEKEN_ENCRYPT win build (verified byte-identical to the Linux ELF)

Without them the pack flow falls back to WSL automatically.
