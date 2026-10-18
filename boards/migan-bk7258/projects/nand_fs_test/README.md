# nand_fs_test

SD-NAND benchmark for the migan-bk7258. The board carries a 1 GB
SD-NAND chip (U7) wired to the bk7258 SDIO host controller in 4-bit
mode (P14 = CLK, P15 = CMD, P16-P19 = D0-D3).

> **STATUS: the chip is electrically silent on this board.** The full
> vendor init flow runs and times out on the first card commands
> (CMD8/CMD55 response timeouts) on every attempt, for 20+ s after
> power-up, with the power rail verified enabled. Details in
> `boards/migan-bk7258/HARDWARE_STATUS.md`.

## What the app does

1. enables the LDO_3V3 rail (P52) that powers the SD-NAND
2. `bk_sd_card_init()` with retries (SD-NAND chips boot their internal
   controller from NAND after power-on and may need ~1 s)
3. on success: reports capacity/CSD info, then benchmarks:
   - **raw blocks**: erase, write 1 MB (32 KB chunks), read back,
     verify, KB/s
   - **FATFS**: mounts volume "1:", mkfs FAT32 if needed, writes and
     reads a 1 MB file (`1:/bench.bin`) with verify, KB/s, free space
4. LED1 (P40): on while busy; fast-blinks if init fails

## Vendor API (driver/driver/sd_card + sdio_host)

```
bk_sd_card_init(void)
bk_sd_card_get_card_size(void)                  /* sectors, 512 B */
bk_sd_card_get_card_info(&sd_card_info_t)
bk_sd_card_read_blocks(buf, addr, n)
bk_sd_card_write_blocks(buf, addr, n)
bk_sd_card_rw_sync(void)
```

FATFS mounts the same chip as volume `"1:"` (`components/fatfs/disk_io.c`,
`DEV_SD` = pdrv 1 = `DISK_NUMBER_SDIO_SD`) when
`CONFIG_FATFS_SDCARD=y`.

## Build / flash

```
cd D:/armino/beken-prj
make build BOARD=migan-bk7258 PROJECT=nand_fs_test

bk_loader.exe download -p 42 -b 1500000 ^
    -i D:/armino/beken-prj/build/migan-bk7258/nand_fs_test/bk7258/all-app.bin -s 0
```

## Config

```
CONFIG_SDIO_HOST=y
CONFIG_SDCARD=y
CONFIG_FATFS=y
CONFIG_FATFS_SDCARD=y
```

`bk_sd_card_init` retried for 20 s (SD-NAND chips may take up to ~1 s
after power-on before responding). P52 (LDO_3V3 enable) is driven high
first — the NAND VDD rail hangs off that LDO.
