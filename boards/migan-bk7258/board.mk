# migan-bk7258 board definition
#
# Hardware:
#   - SoC        : BK7258 (3x Cortex-M33), 16 MB PSRAM, 8 MiB SPI NOR (partitioned)
#   - Console    : CH340 USB-UART on UART0, 115200 8N1
#   - LEDs       : LED1 = P40 (GPIO_40), LED2 = P41 (GPIO_41), both high-active
#
# Projects:
#   - blink_hello : in this repo  (boards/migan-bk7258/projects/blink_hello)
#   - beken_genie : vendor AI solution project (lives inside the vendor tree:
#                   vendor/bk_aidk/projects/beken_genie)

SOC := bk7258

# Flashing tools (native Windows builds from the board database).
# Override in config.local.mk if placed elsewhere.
GEN_IMAGE_WIN_EXE   ?= D:\board_database\main-migan-BK7258\BEKEN_GEN_V1.0.0.5_20221215\win\cmake_Gen_image.exe
ENCRYPT_CRC_WIN_EXE ?= D:\board_database\main-migan-BK7258\BEKEN_ENCRYPT_V5.0.0.7_20231008\win\cmake_encrypt_crc.exe

# CLI flasher (bk_loader.exe lives next to the GUI BKFIL.exe)
BK_LOADER ?= D:\board_database\main-migan-BK7258\BEKEN_BKFIL_V2.1.12.2_20251030\bk_loader.exe
FLASH_BAUD ?= 1500000
COM ?= 42
