/*
 * rec_play - raw 10 s voice recorder / player for the migan-bk7258
 *
 * Mirrors the behavior of esp32-demo/p4-cb-v3_2/app/touch_rec_play,
 * with the touch UI replaced by the board keys:
 *
 *   boot          -> erase the record region (background), only Rec enabled
 *   S1 (rec)      -> fixed 10 s recording, LED1 on, % progress on the log
 *   record done   -> Play enabled
 *   S2 (play)     -> playback, LED2 + speaker PA on, % progress
 *   play done     -> idle again (replay allowed, recording kept)
 *   S3            -> status print
 *   keys are ignored while busy
 *
 * Audio: 16 kHz mono 16-bit PCM through the on-chip codec - ADC MIC1
 * differential, DAC differential to the HT6873 PA. GDMA repeat rings
 * (the aud_tras_drv pattern); half/finish ISRs ping-pong the halves.
 *
 * Storage: raw PCM in the SPI NOR at 0x610000 (320 KB = 10 s), erased
 * at boot so recording never stalls on a sector erase.
 *
 * Hardware (migan schematic):
 *   mic        MICP1/MICN1 (on-chip codec, MICBIAS)
 *   speaker    AUDLP/AUDLN -> HT6873; PA enable (MUTE) = GPIO_50, high = on
 *   keys       S1 = GPIO_13, S2 = GPIO_12, S3 = GPIO_8 (active low)
 *   leds       LED1 = GPIO_40 (recording), LED2 = GPIO_41 (playing)
 */

#include <stdio.h>
#include <string.h>
#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <driver/gpio.h>
#include <driver/aud_adc.h>
#include <driver/aud_dac.h>
#include <driver/dma.h>
#include <driver/flash.h>

/* ---- pins ---- */
#define KEY_REC		GPIO_13		/* S1 */
#define KEY_PLAY	GPIO_12		/* S2 */
#define KEY_SPARE	GPIO_8		/* S3 */
#define LED_REC		GPIO_40		/* LED1 */
#define LED_PLAY	GPIO_41		/* LED2 */
#define PA_MUTE		GPIO_50		/* HT6873 enable, high = play */

/* ---- audio / storage ---- */
#define SAMPLE_RATE	16000
#define REC_SECONDS	10
#define REC_WORDS	(SAMPLE_RATE * REC_SECONDS)		/* 160000 samples */
#define REC_BYTES	(REC_WORDS * 2)				/* 320000 */
#define REC_FLASH_ADDR	0x610000
#define REC_SECTORS	((REC_BYTES + 4095) / 4096)		/* 79 */

/* DMA rings: 32 KB = 8192 x 32-bit words = 8192 samples = 512 ms/pass,
 * half/finish events every 256 ms (one ring word = one 16-bit sample) */
#define RING_WORDS	8192
#define HALF_WORDS	(RING_WORDS / 2)
#define HALF_BYTES	(HALF_WORDS * 2)	/* compacted pcm bytes per half */

#define STAGE_SAMPS	HALF_WORDS		/* 16 KB staging = one ring half */

static uint32_t adc_ring[RING_WORDS];
static uint32_t dac_ring[RING_WORDS];
static uint16_t stage[STAGE_SAMPS];

static dma_id_t adc_dma = DMA_ID_MAX;
static dma_id_t dac_dma = DMA_ID_MAX;
static beken_semaphore_t adc_sem;
static beken_semaphore_t dac_sem;
static volatile int adc_ev[4];		/* 0 = first half, 1 = second half */
static volatile int adc_ev_wr, adc_ev_rd;
static volatile int dac_ev[4];
static volatile int dac_ev_wr, dac_ev_rd;

typedef enum {
	ST_IDLE = 0,
	ST_ERASING,
	ST_RECORDING,
	ST_PLAYING,
} state_t;

static volatile state_t g_state = ST_IDLE;
static volatile bool g_erase_done = false;
static volatile int g_request = 0;	/* 1 = rec, 2 = play */
static bool g_has_recording = false;

/* ---- gpio helpers ---- */
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

static void key_in_init(gpio_id_t g)
{
	gpio_dev_unmap(g);
	bk_gpio_enable_pull(g);
	bk_gpio_pull_up(g);
	bk_gpio_enable_input(g);
}

static int key_pressed(gpio_id_t g)
{
	return bk_gpio_get_input(g) == 0;
}

/* ---- record path ---- */
static void adc_dma_half_isr(void)
{
	adc_ev[adc_ev_wr++ & 3] = 0;
	rtos_set_semaphore(&adc_sem);
}

static void adc_dma_finish_isr(void)
{
	adc_ev[adc_ev_wr++ & 3] = 1;
	rtos_set_semaphore(&adc_sem);
}

static void rec_start(void)
{
	dma_config_t cfg = {0};
	uint32_t fifo = 0;
	aud_adc_config_t adc = DEFAULT_AUD_ADC_CONFIG();

	adc.samp_rate = SAMPLE_RATE;
	bk_aud_driver_init();
	bk_aud_adc_init(&adc);

	bk_dma_driver_init();
	cfg.mode = DMA_WORK_MODE_REPEAT;
	cfg.chan_prio = 1;
	cfg.src.dev = DMA_DEV_AUDIO_RX;
	cfg.dst.dev = DMA_DEV_DTCM;
	cfg.src.width = DMA_DATA_WIDTH_16BITS;		/* mono MIC1 */
	cfg.dst.width = DMA_DATA_WIDTH_32BITS;
	bk_aud_adc_get_fifo_addr(&fifo);
	cfg.src.addr_inc_en = DMA_ADDR_INC_ENABLE;
	cfg.src.addr_loop_en = DMA_ADDR_LOOP_ENABLE;
	cfg.src.start_addr = fifo;
	cfg.src.end_addr = fifo + 4;
	cfg.dst.addr_inc_en = DMA_ADDR_INC_ENABLE;
	cfg.dst.addr_loop_en = DMA_ADDR_LOOP_ENABLE;
	cfg.dst.start_addr = (uint32_t)adc_ring;
	cfg.dst.end_addr = (uint32_t)adc_ring + sizeof(adc_ring);

	adc_dma = bk_dma_alloc(DMA_DEV_AUDIO);
	bk_dma_init(adc_dma, &cfg);
	bk_dma_set_transfer_len(adc_dma, sizeof(adc_ring));
	bk_dma_register_isr(adc_dma, adc_dma_half_isr, adc_dma_finish_isr);
	bk_dma_enable_half_finish_interrupt(adc_dma);
	bk_dma_enable_finish_interrupt(adc_dma);

	adc_ev_wr = adc_ev_rd = 0;
	bk_dma_start(adc_dma);
	bk_aud_adc_start();
}

static void rec_stop(void)
{
	bk_aud_adc_stop();
	bk_dma_stop(adc_dma);
	bk_dma_deinit(adc_dma);
	bk_dma_free(DMA_DEV_AUDIO, adc_dma);
	bk_aud_adc_deinit();
	bk_aud_driver_deinit();
}

/* drain one ring half into flash at `addr` as 16-bit pcm; returns words */
static uint32_t rec_drain(int which, uint32_t flash_addr, uint32_t words_left)
{
	uint32_t *src = which ? &adc_ring[HALF_WORDS] : &adc_ring[0];
	uint32_t words = (words_left < HALF_WORDS) ? words_left : HALF_WORDS;

	for (uint32_t i = 0; i < words; i++)
		stage[i] = (uint16_t)src[i];

	bk_flash_write_bytes(flash_addr, (uint8_t *)stage, words * 2);

	return words;
}

static void do_record(void)
{
	uint32_t flash_addr = REC_FLASH_ADDR;
	uint32_t left = REC_WORDS;
	uint32_t done = 0;
	int last_pct = -1;

	printf("rec: start (%d s, %d bytes)\r\n", REC_SECONDS, REC_BYTES);
	bk_gpio_set_output_high(LED_REC);
	rec_start();

	while (done < left) {
		rtos_get_semaphore(&adc_sem, BEKEN_NEVER_TIMEOUT);
		while (adc_ev_rd != adc_ev_wr && done < left) {
			done += rec_drain(adc_ev[adc_ev_rd++ & 3], flash_addr, left - done);
			flash_addr += done < left ? HALF_BYTES : 0;
		}
		int pct = (int)(100ULL * done / left);
		if (pct != last_pct && (pct % 10) == 0) {
			printf("rec: %d%%\r\n", pct);
			last_pct = pct;
		}
	}

	/* let the in-flight half finish writing, then stop */
	rec_stop();
	bk_gpio_set_output_low(LED_REC);
	g_has_recording = true;
	printf("rec: done (%u bytes @ 0x%x)\r\n", done * 4, REC_FLASH_ADDR);
}

/* ---- playback path ---- */
static void dac_dma_half_isr(void)
{
	dac_ev[dac_ev_wr++ & 3] = 0;
	rtos_set_semaphore(&dac_sem);
}

static void dac_dma_finish_isr(void)
{
	dac_ev[dac_ev_wr++ & 3] = 1;
	rtos_set_semaphore(&dac_sem);
}

static void play_start(void)
{
	dma_config_t cfg = {0};
	uint32_t fifo = 0;
	aud_dac_config_t dac = DEFAULT_AUD_DAC_CONFIG();

	dac.samp_rate = SAMPLE_RATE;
	bk_aud_driver_init();
	bk_aud_dac_init(&dac);

	bk_dma_driver_init();
	cfg.mode = DMA_WORK_MODE_REPEAT;
	cfg.chan_prio = 1;
	cfg.src.dev = DMA_DEV_DTCM;
	cfg.dst.dev = DMA_DEV_AUDIO;
	cfg.src.width = DMA_DATA_WIDTH_32BITS;
	cfg.dst.width = DMA_DATA_WIDTH_16BITS;		/* left channel mono */
	bk_aud_dac_get_fifo_addr(&fifo);
	cfg.dst.addr_inc_en = DMA_ADDR_INC_ENABLE;
	cfg.dst.addr_loop_en = DMA_ADDR_LOOP_ENABLE;
	cfg.dst.start_addr = fifo;
	cfg.dst.end_addr = fifo + 4;
	cfg.src.addr_inc_en = DMA_ADDR_INC_ENABLE;
	cfg.src.addr_loop_en = DMA_ADDR_LOOP_ENABLE;
	cfg.src.start_addr = (uint32_t)dac_ring;
	cfg.src.end_addr = (uint32_t)dac_ring + sizeof(dac_ring);

	dac_dma = bk_dma_alloc(DMA_DEV_AUDIO);
	bk_dma_init(dac_dma, &cfg);
	bk_dma_set_transfer_len(dac_dma, sizeof(dac_ring));
	bk_dma_register_isr(dac_dma, dac_dma_half_isr, dac_dma_finish_isr);
	bk_dma_enable_half_finish_interrupt(dac_dma);
	bk_dma_enable_finish_interrupt(dac_dma);

	dac_ev_wr = dac_ev_rd = 0;
	bk_dma_start(dac_dma);
	bk_aud_dac_start();
	bk_gpio_set_output_high(PA_MUTE);		/* speaker on */
}

static void play_stop(void)
{
	bk_gpio_set_output_low(PA_MUTE);
	bk_aud_dac_stop();
	bk_dma_stop(dac_dma);
	bk_dma_deinit(dac_dma);
	bk_dma_free(DMA_DEV_AUDIO, dac_dma);
	bk_aud_dac_deinit();
	bk_aud_driver_deinit();
}

/* fill one ring half from 16-bit pcm in flash; returns words filled */
static uint32_t play_fill(int which, uint32_t flash_addr, uint32_t words_left)
{
	uint32_t *dst = which ? &dac_ring[HALF_WORDS] : &dac_ring[0];
	uint32_t words = (words_left < HALF_WORDS) ? words_left : HALF_WORDS;

	if (words) {
		bk_flash_read_bytes(flash_addr, (uint8_t *)stage, words * 2);
		for (uint32_t i = 0; i < words; i++)
			dst[i] = (uint32_t)stage[i];
	}
	if (words < HALF_WORDS)
		memset(&dst[words], 0, (HALF_WORDS - words) * 4);

	return words;
}

static void do_play(void)
{
	uint32_t flash_addr = REC_FLASH_ADDR;
	uint32_t left = REC_WORDS;
	uint32_t done = 0;
	int last_pct = -1;

	printf("play: start\r\n");
	bk_gpio_set_output_high(LED_PLAY);

	/* pre-fill both halves before the DMA starts pulling */
	done += play_fill(0, flash_addr, left);
	flash_addr += HALF_BYTES;
	done += play_fill(1, flash_addr, left - done);
	flash_addr += HALF_BYTES;

	play_start();

	while (done < left) {
		rtos_get_semaphore(&dac_sem, BEKEN_NEVER_TIMEOUT);
		while (dac_ev_rd != dac_ev_wr && done < left) {
			done += play_fill(dac_ev[dac_ev_rd++ & 3], flash_addr, left - done);
			flash_addr += done < left ? HALF_BYTES : 0;
		}
		int pct = (int)(100ULL * done / left);
		if (pct != last_pct && (pct % 10) == 0) {
			printf("play: %d%%\r\n", pct);
			last_pct = pct;
		}
	}

	/* let the last samples drain out of the ring before muting */
	rtos_delay_milliseconds(600);
	play_stop();
	bk_gpio_set_output_low(LED_PLAY);
	printf("play: done\r\n");
}

/* ---- boot erase of the record region ---- */
static void erase_region_task(beken_thread_arg_t arg)
{
	(void)arg;

	g_state = ST_ERASING;
	printf("rec: erasing %d sectors @ 0x%x...\r\n", REC_SECTORS, REC_FLASH_ADDR);
	bk_flash_driver_init();
	for (int i = 0; i < REC_SECTORS; i++)
		bk_flash_erase_sector(REC_FLASH_ADDR + i * 4096);
	g_erase_done = true;
	g_state = ST_IDLE;
	printf("rec: region ready, press S1 to record\r\n");
	rtos_delete_thread(NULL);
}

/* ---- keys ---- */
static void key_task(beken_thread_arg_t arg)
{
	(void)arg;
	int rec_hist = 1, play_hist = 1;

	while (1) {
		int rec_now = key_pressed(KEY_REC);
		int play_now = key_pressed(KEY_PLAY);

		if (g_state == ST_IDLE && g_erase_done) {
			if (rec_now && !rec_hist)
				g_request = 1;
			else if (play_now && !play_hist && g_has_recording)
				g_request = 2;
		}

		rec_hist = rec_now;
		play_hist = play_now;

		if (key_pressed(KEY_SPARE))
			printf("state: %d, has_rec=%d, erase_done=%d (S1=rec S2=play)\r\n",
				g_state, g_has_recording, g_erase_done);

		rtos_delay_milliseconds(25);
	}
}

/* ---- app task ---- */
static void app_task(beken_thread_arg_t arg)
{
	(void)arg;

	while (1) {
		if (g_request == 1 && g_state == ST_IDLE) {
			g_request = 0;
			g_state = ST_RECORDING;
			do_record();
			g_state = ST_IDLE;
		} else if (g_request == 2 && g_state == ST_IDLE) {
			g_request = 0;
			g_state = ST_PLAYING;
			do_play();
			g_state = ST_IDLE;
		}
		rtos_delay_milliseconds(20);
	}
}

void user_app_main(void)
{
#if (CONFIG_SYS_CPU0)
	printf("\r\n===== rec_play on BK7258 (16k mono, 10 s, flash) =====\r\n");

	bk_gpio_driver_init();
	gpio_out_init(LED_REC, 0);
	gpio_out_init(LED_PLAY, 0);
	gpio_out_init(PA_MUTE, 0);		/* PA muted */
	key_in_init(KEY_REC);
	key_in_init(KEY_PLAY);
	key_in_init(KEY_SPARE);

	rtos_init_semaphore_ex(&adc_sem, 1, 0);
	rtos_init_semaphore_ex(&dac_sem, 1, 0);

	rtos_create_thread(NULL, 4, "erase", erase_region_task, 4096, 0);
	rtos_create_thread(NULL, 4, "keys", key_task, 4096, 0);
	rtos_create_thread(NULL, 5, "app", app_task, 8192, 0);
#else
	printf("rec_play: idle on this core\r\n");
#endif
}

int main(void)
{
	rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
	bk_init();

	return 0;
}
