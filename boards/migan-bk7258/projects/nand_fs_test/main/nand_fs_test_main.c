/*
 * nand_fs_test - SD-NAND raw + FATFS benchmark for the migan-bk7258
 *
 * The board carries a 1 GB SD-NAND wired to the SDIO host controller
 * (P14 = CLK, P15 = CMD, P16-P19 = D0-D3). The vendor driver presents
 * it as an SD card (bk_sd_card_*), and FATFS mounts it as volume "1:".
 *
 * The test:
 *   1. init + report capacity
 *   2. raw block benchmark: write + read 1 MB with verify, KB/s
 *   3. FATFS benchmark: mkfs if needed, write + read a 1 MB file
 *      with verify, KB/s, free space
 *
 * All I/O is 32 KB chunks (64 x 512 B blocks).
 */

#include <stdio.h>
#include <string.h>

#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <driver/gpio.h>
#include <driver/sd_card.h>
#include <driver/aon_rtc.h>
#include "ff.h"
#include "diskio.h"

#if (CONFIG_SYS_CPU0)

#define LED_HB		GPIO_40

#define CHUNK_SECTORS	64			/* 64 x 512 B = 32 KB */
#define SECTOR_SIZE	512
#define BENCH_SECTORS	2048			/* 1 MB */
#define FAT_BENCH_BYTES	(1024 * 1024)		/* 1 MB file */

static uint8_t wbuf[CHUNK_SECTORS * SECTOR_SIZE];
static uint8_t rbuf[CHUNK_SECTORS * SECTOR_SIZE];
static uint8_t expect[CHUNK_SECTORS * SECTOR_SIZE];
static FATFS s_fs;

static uint32_t now_ms(void)
{
	return (uint32_t)(bk_aon_rtc_get_us() / 1000);
}

static uint32_t kb_per_s(uint32_t kb, uint32_t ms)
{
	return ms ? kb * 1000 / ms : 0;
}

static void gpio_out_init(gpio_id_t g, int level)
{
	gpio_dev_unmap(g);
	bk_gpio_disable_pull(g);
	bk_gpio_enable_output(g);
	if (level)
		bk_gpio_set_output_high(g);
	else
		bk_gpio_set_output_low(g);
}

static void fill_pattern(uint8_t *buf, uint32_t bytes, uint32_t salt)
{
	for (uint32_t i = 0; i < bytes; i++)
		buf[i] = (uint8_t)((i * 7 + salt + (i >> 9)) & 0xFF);
}

/* ---------- stage 1: raw block benchmark ---------- */
static int raw_bench(void)
{
	uint32_t sectors = bk_sd_card_get_card_size();
	sd_card_info_t info = {0};

	printf("nand: capacity %u sectors (%u MB)\r\n",
		sectors, sectors * 512 / 1024 / 1024);

	if (bk_sd_card_get_card_info(&info) == BK_OK)
		printf("nand: ver %u type %u class %u rca %u\r\n",
			info.card_version, info.card_type, info.class,
			info.relative_card_addr);

	if (sectors < BENCH_SECTORS) {
		printf("nand: too small for the benchmark\r\n");
		return -1;
	}

	printf("raw: erasing %d sectors...\r\n", BENCH_SECTORS);
	bk_sd_card_erase(0, BENCH_SECTORS);
	while (bk_sd_card_get_card_state() != SD_CARD_TRANSFER)
		rtos_delay_milliseconds(1);

	/* write */
	uint32_t t0 = now_ms();
	for (uint32_t off = 0; off < BENCH_SECTORS; off += CHUNK_SECTORS) {
		fill_pattern(wbuf, sizeof(wbuf), off);
		if (bk_sd_card_write_blocks(wbuf, off, CHUNK_SECTORS) != BK_OK) {
			printf("raw: write failed @%u\r\n", off);
			return -1;
		}
	}
	bk_sd_card_rw_sync();
	uint32_t wt = now_ms() - t0;

	/* read + verify */
	uint32_t errors = 0;
	t0 = now_ms();
	for (uint32_t off = 0; off < BENCH_SECTORS; off += CHUNK_SECTORS) {
		if (bk_sd_card_read_blocks(rbuf, off, CHUNK_SECTORS) != BK_OK) {
			printf("raw: read failed @%u\r\n", off);
			errors++;
			break;
		}
		fill_pattern(expect, sizeof(expect), off);
		if (memcmp(rbuf, expect, sizeof(expect)) != 0)
			errors++;
	}
	uint32_t rt = now_ms() - t0;

	if (errors) {
		printf("raw: VERIFY FAILED (%u errors)\r\n", errors);
		return -1;
	}

	printf("raw: WRITE %u KB in %u ms = %u KB/s\r\n",
		BENCH_SECTORS / 2, wt, kb_per_s(BENCH_SECTORS / 2, wt));
	printf("raw: READ  %u KB in %u ms = %u KB/s\r\n",
		BENCH_SECTORS / 2, rt, kb_per_s(BENCH_SECTORS / 2, rt));
	printf("raw: verify OK\r\n");
	return 0;
}

/* ---------- stage 2: FATFS benchmark ---------- */
static void fat_bench(void)
{
	FATFS fs = {0};
	FIL fil = {0};
	FRESULT fr;
	static TCHAR work[4096];

	fr = f_mount(&s_fs, "1:", 1);
	if (fr == FR_NO_FILESYSTEM) {
		printf("fat: no filesystem, mkfs FAT32...\r\n");
		fr = f_mkfs("1:", FM_FAT32, 0, work, sizeof(work));
		if (fr != FR_OK) {
			printf("fat: mkfs failed %d\r\n", fr);
			return;
		}
		fr = f_mount(&s_fs, "1:", 1);
	}
	if (fr != FR_OK) {
		printf("fat: mount failed %d\r\n", fr);
		return;
	}

	uint32_t free_cl = 0;
	FATFS *fsp = &s_fs;
	if (f_getfree("1:", &free_cl, &fsp) == FR_OK)
		printf("fat: %u MB free\r\n", free_cl * 32 / 1024);

	/* write 1 MB file */
	fr = f_open(&fil, "1:/bench.bin", FA_CREATE_ALWAYS | FA_WRITE);
	if (fr != FR_OK) {
		printf("fat: open write failed %d\r\n", fr);
		return;
	}

	fill_pattern(wbuf, sizeof(wbuf), 0x5A);
	uint32_t t0 = now_ms();
	UINT bw = 0;

	for (uint32_t off = 0; off < FAT_BENCH_BYTES; off += sizeof(wbuf)) {
		fr = f_write(&fil, wbuf, sizeof(wbuf), &bw);
		if (fr != FR_OK || bw != sizeof(wbuf)) {
			printf("fat: write failed %d (%u)\r\n", fr, bw);
			f_close(&fil);
			return;
		}
	}
	f_sync(&fil);
	uint32_t wt = now_ms() - t0;
	f_close(&fil);
	printf("fat: WRITE %u KB in %u ms = %u KB/s\r\n",
		FAT_BENCH_BYTES / 1024, wt, kb_per_s(FAT_BENCH_BYTES / 1024, wt));

	/* read back + verify */
	fr = f_open(&fil, "1:/bench.bin", FA_READ);
	if (fr != FR_OK) {
		printf("fat: open read failed %d\r\n", fr);
		return;
	}

	fill_pattern(expect, sizeof(expect), 0x5A);
	uint32_t errors = 0, done = 0;
	t0 = now_ms();

	while (done < FAT_BENCH_BYTES) {
		UINT br = 0;
		fr = f_read(&fil, rbuf, sizeof(rbuf), &br);
		if (fr != FR_OK || br == 0)
			break;
		if (memcmp(rbuf, expect, br) != 0)
			errors++;
		done += br;
	}
	uint32_t rt = now_ms() - t0;
	f_close(&fil);

	printf("fat: READ %u KB in %u ms = %u KB/s\r\n",
		done / 1024, rt, kb_per_s(done / 1024, rt));
	printf("fat: %s\r\n", errors ? "VERIFY FAILED" : "verify OK");
}

void user_app_main(void)
{
	gpio_out_init(LED_HB, 1);		/* on = busy */

	printf("\r\n===== nand_fs_test on BK7258 (SD-NAND, SDIO 4-line) =====\r\n");

	/* the SD-NAND's VDD hangs off LDO_3V3, which is enabled by
	 * P52 (LDO33_EN) - same rail as the LCD modules */
	gpio_out_init(GPIO_52, 1);
	rtos_delay_milliseconds(10);
	printf("nand: LDO_3V3 (P52) on\r\n");

	/* the SD-NAND boots its internal controller from NAND after power
	 * on and may need up to a second before SCCB.. er, CMD responses
	 * arrive - retry init for up to 10 s */
	int err = BK_FAIL;

	for (int i = 1; i <= 20; i++) {
		err = bk_sd_card_init();
		printf("nand: init try %d ret=%d\r\n", i, err);
		if (err == BK_OK)
			break;
		rtos_delay_milliseconds(500);
	}

	if (err == BK_OK)
		raw_bench();
	else
		printf("nand: INIT FAILED after retries\r\n");

	fat_bench();

	gpio_out_init(LED_HB, 0);
	printf("nand: test complete\r\n");

	while (1)
		rtos_delay_milliseconds(1000);
}

#else /* !CONFIG_SYS_CPU0 */
void user_app_main(void)
{
}
#endif

int main(void)
{
#if (CONFIG_SYS_CPU0)
	rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
#endif
	bk_init();

	return 0;
}
