/*
 * rec_play - raw 10 s voice recorder / player for the migan-bk7258
 *
 * Button-driven port of esp32-demo/p4-cb-v3_2/app/touch_rec_play:
 *   boot          -> LCD banner, only Rec enabled
 *   S1            -> fixed 10 s recording into PSRAM, LED1, progress bar
 *   record done   -> Play enabled
 *   S2            -> playback, LED2 + speaker PA on, progress bar
 *   done          -> idle again (replay allowed)
 *   S3            -> status print; keys ignored while busy
 *
 * Audio: 16 kHz mono 16-bit via the on-chip codec - ADC MIC1 differential,
 * DAC differential -> HT6873 PA (enable = GPIO_50). GDMA repeat rings
 * (aud_tras_drv pattern: 8 KB ring = pass, finish IRQ per 128 ms).
 * clk_src = AUD_CLK_XTAL (matches the production mic config; the APLL
 * default does not clock the ADC here and the DMA never fires).
 *
 * Storage: one 320 KB buffer from the PSRAM heap (psram_malloc) - no
 * flash, nothing to erase.
 *
 * GUI: the two round GC9D01 panels (mirrored), driven the dual_GC9D01
 * way: one-time init over the SW bit-bang, per-frame pushes over the
 * QSPI units at 480M/7 (~25 ms). Shows REC/PLAY buttons and a progress
 * bar.
 *
 * Pins (migan schematic):
 *   LCD1: SCLK=P2  CS=P3  MOSI=P4  DC=P5  RST=P45   (QSPI1)
 *   LCD2: SCLK=P22 CS=P23 MOSI=P24 DC=P7  RST=P6    (QSPI0)
 *   panel power P52=LDO33_EN, backlight P25=PWM5 @ 30%
 *   keys S1=GPIO_13 S2=GPIO_12 S3=GPIO_8 (active low)
 *   LED1=P40 (rec) LED2=P41 (play), PA=P50
 */

#include <stdio.h>
#include <string.h>
#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <os/mem.h>
#include <driver/gpio.h>
#include <driver/pwm.h>
#include <driver/aud_adc.h>
#include <driver/aud_dac.h>
#include <driver/dma.h>
#include <driver/qspi.h>
#include <driver/qspi_types.h>
#include "gpio_ll.h"
#include "qspi_hal.h"
#include "qspi_ll.h"

#if (CONFIG_SYS_CPU0)

/* ---- pins ---- */
#define KEY_REC		GPIO_13		/* S1 */
#define KEY_PLAY	GPIO_12		/* S2 */
#define KEY_SPARE	GPIO_8		/* S3 */
#define LED_REC		GPIO_40
#define LED_PLAY	GPIO_41
#define PA_MUTE		GPIO_50		/* HT6873 enable, high = on */
#define PANEL_LDO	GPIO_52		/* LDO33_EN */

/* LCD1 */
#define SCK1	GPIO_2
#define CS1	GPIO_3
#define MOSI1	GPIO_4
#define DC1	GPIO_5
#define RST1	GPIO_45
/* LCD2 */
#define SCK2	GPIO_22
#define CS2	GPIO_23
#define MOSI2	GPIO_24
#define DC2	GPIO_7
#define RST2	GPIO_6

/* backlight */
#define BL_PWM_CH	5
#define BL_PWM_HZ	1000
#define BL_DUTY_PCT	30
#define PWM_SRC_CLK_HZ	26000000

/* ---- audio ---- */
#define SAMPLE_RATE	16000
#define REC_SECONDS	10
#define REC_WORDS	(SAMPLE_RATE * REC_SECONDS)	/* 160000 samples */
#define REC_BYTES	(REC_WORDS * 2)			/* 320000 */

/* DMA ring: 8 KB = 2048 x 32-bit words = 2048 samples = 128 ms/pass
 * (one ring word = one 16-bit sample; finish IRQ per pass) */
#define RING_WORDS	2048
#define RING_BYTES	(RING_WORDS * 4)

static uint32_t adc_ring[RING_WORDS];
static uint32_t dac_ring[RING_WORDS];
static uint16_t *rec_buf;			/* 320 KB in PSRAM */

static dma_id_t adc_dma = DMA_ID_MAX;
static dma_id_t dac_dma = DMA_ID_MAX;
static beken_semaphore_t adc_sem;
static beken_semaphore_t dac_sem;

/* ==================== LCD (dual_GC9D01 driver, both panels) ============ */

static gpio_hw_t *const ghw = (gpio_hw_t *)GPIO_LL_REG_BASE;
#define FV(g, v)	gpio_ll_set_output_value(ghw, (g), (v))
#define FH(g)		FV((g), 1)
#define FL(g)		FV((g), 0)

typedef struct {
	volatile uint32_t *reg;
	uint32_t lo, hi;
} fpin_t;

static fpin_t fp_sck1, fp_sck2, fp_mosi1, fp_mosi2, fp_cs1, fp_cs2, fp_dc1, fp_dc2;

static void fp_init(fpin_t *p, gpio_id_t g)
{
	p->reg = (volatile uint32_t *)&ghw->gpio_num[g];
	FV(g, 0);
	p->lo = *p->reg;
	FV(g, 1);
	p->hi = *p->reg;
}

#define P_LO(p)	(*p.reg = p.lo)
#define P_HI(p)	(*p.reg = p.hi)

#define CFG_2ND_FUNC	BIT(6)
static qspi_hal_t hw_hal[2];
static uint32_t hw_w_sck1, hw_w_sck2, hw_w_mosi1, hw_w_mosi2, hw_w_cs1, hw_w_cs2;

static int hw_wait(int unit)
{
	qspi_hw_t *hw = hw_hal[unit].hw;

	for (uint32_t i = 0; i < 20000; i++) {
		if (qspi_ll_is_cmd_start_done(hw))
			return 0;
	}
	hw->status_clr.clr_cmd_start_done = 1;
	hw->status_clr.clr_cmd_start_done = 0;
	return -1;
}

static int hw_send(int unit, uint32_t value, uint32_t nbytes)
{
	qspi_hal_t *h = &hw_hal[unit];

	qspi_hal_set_cmd_c_l(h, 0);
	qspi_hal_set_cmd_c_h(h, value);
	qspi_hal_set_cmd_c_cfg1(h, 0x3 << (nbytes * 2));
	qspi_hal_set_cmd_c_cfg2(h, 0);
	qspi_hal_cmd_c_start(h);
	return hw_wait(unit);
}

static void hw_dc_lo(void)	{ *fp_dc1.reg = fp_dc1.lo; *fp_dc2.reg = fp_dc2.lo; }
static void hw_dc_hi(void)	{ *fp_dc1.reg = fp_dc1.hi; *fp_dc2.reg = fp_dc2.hi; }

static void pads_to_hw(void)
{
	*fp_sck1.reg = hw_w_sck1;	*fp_sck2.reg = hw_w_sck2;
	*fp_mosi1.reg = hw_w_mosi1;	*fp_mosi2.reg = hw_w_mosi2;
	*fp_cs1.reg = hw_w_cs1;		*fp_cs2.reg = hw_w_cs2;
}

static void pads_to_sw(void)
{
	*fp_sck1.reg = fp_sck1.hi;	*fp_sck2.reg = fp_sck2.hi;
	*fp_mosi1.reg = fp_mosi1.hi;	*fp_mosi2.reg = fp_mosi2.hi;
	*fp_cs1.reg = fp_cs1.hi;	*fp_cs2.reg = fp_cs2.hi;
}

static void hw_init(void)
{
	qspi_config_t cfg = {0};

	bk_qspi_driver_init();
	cfg.src_clk = QSPI_SCLK_480M;
	cfg.src_clk_div = 7;

	for (int u = 0; u < 2; u++) {
		hw_hal[u].id = (qspi_id_t)u;
		qspi_hal_init(&hw_hal[u]);
		bk_qspi_init((qspi_id_t)u, &cfg);
	}

	hw_w_sck1  = fp_sck1.hi  | CFG_2ND_FUNC;
	hw_w_sck2  = fp_sck2.hi  | CFG_2ND_FUNC;
	hw_w_mosi1 = fp_mosi1.hi | CFG_2ND_FUNC;
	hw_w_mosi2 = fp_mosi2.hi | CFG_2ND_FUNC;
	hw_w_cs1   = fp_cs1.hi   | CFG_2ND_FUNC;
	hw_w_cs2   = fp_cs2.hi   | CFG_2ND_FUNC;
}

/* ---- SW bit-bang (init path) ---- */
static void spi_byte(uint8_t b)
{
	for (int i = 0; i < 8; i++) {
		if (b & 0x80) { P_HI(fp_mosi1); P_HI(fp_mosi2); } else { P_LO(fp_mosi1); P_LO(fp_mosi2); }
		P_LO(fp_sck1); P_LO(fp_sck2);
		P_HI(fp_sck1); P_HI(fp_sck2);
		b <<= 1;
	}
}

static void tx_begin(void)	{ P_LO(fp_cs1); P_LO(fp_cs2); }
static void tx_end(void)	{ P_HI(fp_cs1); P_HI(fp_cs2); }
static void tx_mode_cmd(void)	{ P_LO(fp_dc1); P_LO(fp_dc2); }
static void tx_mode_data(void)	{ P_HI(fp_dc1); P_HI(fp_dc2); }

static void lcd_cmd_data(uint8_t cmd, const uint8_t *data, uint32_t len)
{
	tx_begin();
	tx_mode_cmd();
	spi_byte(cmd);
	tx_mode_data();
	for (uint32_t i = 0; i < len; i++)
		spi_byte(data[i]);
	tx_end();
}

static const uint8_t gc_init[][34] = {
	{0xFE, 0},
	{0xEF, 0},
	{0x80, 1, 0xFF}, {0x81, 1, 0xFF}, {0x82, 1, 0xFF}, {0x83, 1, 0xFF},
	{0x84, 1, 0xFF}, {0x85, 1, 0xFF}, {0x86, 1, 0xFF}, {0x87, 1, 0xFF},
	{0x88, 1, 0xFF}, {0x89, 1, 0xFF}, {0x8A, 1, 0xFF}, {0x8B, 1, 0xFF},
	{0x8C, 1, 0xFF}, {0x8D, 1, 0xFF}, {0x8E, 1, 0xFF}, {0x8F, 1, 0xFF},
	{0x3A, 1, 0x05},
	{0xEC, 1, 0x01},
	{0x74, 7, 0x02, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x98, 3, 0x3E, 0x99, 0x3E},
	{0xB5, 2, 0x0D, 0x0D},
	{0x60, 4, 0x38, 0x0F, 0x79, 0x67},
	{0x61, 4, 0x38, 0x11, 0x79, 0x67},
	{0x64, 6, 0x38, 0x17, 0x71, 0x5F, 0x79, 0x67},
	{0x65, 6, 0x38, 0x13, 0x71, 0x5B, 0x79, 0x67},
	{0x6A, 2, 0x00, 0x00},
	{0x6C, 7, 0x22, 0x02, 0x22, 0x02, 0x22, 0x22, 0x50},
	{0x6E, 32, 0x03, 0x03, 0x01, 0x01, 0x00, 0x00, 0x0F, 0x0F,
	      0x0D, 0x0D, 0x0B, 0x0B, 0x09, 0x09, 0x00, 0x00,
	      0x00, 0x00, 0x0A, 0x0A, 0x0C, 0x0C, 0x0E, 0x0E,
	      0x10, 0x10, 0x00, 0x00, 0x02, 0x02, 0x04, 0x04},
	{0xBF, 1, 0x01},
	{0xF9, 1, 0x40},
	{0x9B, 1, 0x3B},
	{0x93, 3, 0x33, 0x7F, 0x00},
	{0x7E, 1, 0x30},
	{0x70, 6, 0x0D, 0x02, 0x08, 0x0D, 0x02, 0x08},
	{0x71, 3, 0x0D, 0x02, 0x08},
	{0x91, 2, 0x0E, 0x09},
	{0xC3, 1, 0x18},
	{0xC4, 1, 0x18},
	{0xC9, 1, 0x3C},
	{0xF0, 6, 0x13, 0x15, 0x04, 0x05, 0x01, 0x38},
	{0xF2, 6, 0x13, 0x15, 0x04, 0x05, 0x01, 0x34},
	{0xF1, 6, 0x4B, 0xB8, 0x7B, 0x34, 0x35, 0xEF},
	{0xF3, 6, 0x47, 0xB4, 0x72, 0x34, 0x35, 0xDA},
	{0x36, 1, 0x00},
	{0x34, 0},
	{0x11, 0},
	{0x00, 0x80, 120},
	{0x29, 0},
};

#define GC_DELAY	0x80

static void lcd_init_panels(void)
{
	FL(RST1); FL(RST2);
	rtos_delay_milliseconds(20);
	FH(RST1); FH(RST2);
	rtos_delay_milliseconds(150);

	for (uint32_t i = 0; i < sizeof(gc_init) / sizeof(gc_init[0]); i++) {
		const uint8_t *e = gc_init[i];

		if (e[1] == GC_DELAY) {
			rtos_delay_milliseconds(e[2]);
			continue;
		}
		lcd_cmd_data(e[0], &e[2], e[1]);
	}
}

/* ---- framebuffer + font (5x7) ---- */
#define LCD_W	160
#define LCD_H	160
static uint16_t fb[LCD_W * LCD_H];

static const uint8_t font5x7[95][5] = {
	{0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00},
	{0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
	{0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
	{0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
	{0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00},
	{0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
	{0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08},
	{0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
	{0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
	{0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
	{0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
	{0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
	{0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
	{0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
	{0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
	{0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06},
	{0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E},
	{0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
	{0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41},
	{0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A},
	{0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
	{0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
	{0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x0C,0x02,0x7F},
	{0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
	{0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E},
	{0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
	{0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
	{0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F},
	{0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07},
	{0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
	{0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00},
	{0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40},
	{0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
	{0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20},
	{0x38,0x44,0x44,0x48,0x7F}, {0x38,0x54,0x54,0x54,0x18},
	{0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
	{0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00},
	{0x20,0x40,0x44,0x3D,0x00}, {0x7F,0x10,0x28,0x44,0x00},
	{0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78},
	{0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38},
	{0x7C,0x14,0x14,0x14,0x08}, {0x08,0x14,0x14,0x18,0x7C},
	{0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
	{0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C},
	{0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x30,0x40,0x3C},
	{0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C},
	{0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00},
	{0x00,0x00,0x7F,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00},
	{0x08,0x08,0x2A,0x1C,0x08},
};

static void fb_fill(uint16_t c)
{
	for (uint32_t i = 0; i < LCD_W * LCD_H; i++)
		fb[i] = c;
}

static void fb_px(uint32_t x, uint32_t y, uint16_t c)
{
	if (x < LCD_W && y < LCD_H)
		fb[y * LCD_W + x] = c;
}

static void fb_draw_char(uint32_t x, uint32_t y, char ch, uint16_t color, uint32_t scale)
{
	if (ch < 0x20 || ch > 0x7E)
		ch = '?';
	const uint8_t *g = font5x7[ch - 0x20];

	for (uint32_t col = 0; col < 5; col++) {
		uint8_t bits = g[col];
		for (uint32_t row = 0; row < 7; row++) {
			if (!(bits & (1u << row)))
				continue;
			for (uint32_t sx = 0; sx < scale; sx++)
				for (uint32_t sy = 0; sy < scale; sy++)
					fb_px(x + col * scale + sx, y + row * scale + sy, color);
		}
	}
}

static void fb_draw_str_c(uint32_t y, const char *s, uint16_t color, uint32_t scale)
{
	uint32_t w = (uint32_t)strlen(s) * 6 * scale;

	w = (LCD_W > w) ? (LCD_W - w) / 2 : 0;
	for (; *s; s++) {
		fb_draw_char(w, y, *s, color, scale);
		w += 6 * scale;
	}
}

static void fb_circle(uint16_t c)
{
	int32_t x = 78, y = 0, err = 0;

	while (x >= y) {
		fb_px(80 + x, 80 + y, c); fb_px(80 + y, 80 + x, c);
		fb_px(80 - y, 80 + x, c); fb_px(80 - x, 80 + y, c);
		fb_px(80 - x, 80 - y, c); fb_px(80 - y, 80 - x, c);
		fb_px(80 + y, 80 - x, c); fb_px(80 + x, 80 - y, c);
		y++;
		err += 1 + 2 * y;
		if (2 * (err - x) + 1 > 0) { x--; err += 1 - 2 * x; }
	}
}

static void fb_bar(uint32_t pct, uint16_t c)
{
	uint32_t w = 112 * pct / 100;

	for (uint32_t x = 0; x < 112; x++)
		for (uint32_t y = 0; y < 14; y++)
			fb_px(24 + x, 88 + y, (x < w) ? c : 0x2104);
}

/* push the fb to both panels over the QSPI units */
static void ui_flush(void)
{
	for (int unit = 0; unit < 2; unit++) {
		hw_dc_lo();
		hw_send(unit, 0x2A, 1);
		hw_dc_hi();
		hw_send(unit, (uint32_t)(LCD_W - 1) << 24, 4);
		hw_dc_lo();
		hw_send(unit, 0x2B, 1);
		hw_dc_hi();
		hw_send(unit, (uint32_t)(LCD_H - 1) << 24, 4);
		hw_dc_lo();
		hw_send(unit, 0x2C, 1);
		hw_dc_hi();
		for (uint32_t i = 0; i < LCD_W * LCD_H; i += 2) {
			uint32_t v = (uint32_t)(fb[i] >> 8)
				   | ((uint32_t)(fb[i] & 0xFF) << 8)
				   | ((uint32_t)(fb[i + 1] >> 8) << 16)
				   | ((uint32_t)(fb[i + 1] & 0xFF) << 24);
			hw_send(unit, v, 4);
		}
	}
}

/* ==================== UI states ==================== */

typedef enum {
	ST_BOOT = 0,
	ST_IDLE,
	ST_RECORDING,
	ST_PLAYING,
} state_t;

static volatile state_t g_state = ST_BOOT;
static volatile int g_request = 0;
static volatile int g_ui_pct = 0;
static bool g_has_recording = false;
static bool g_lcd_ok = true;

static void ui_draw(void)
{
	char line[16];

	if (!g_lcd_ok)
		return;

	fb_fill(0x0000);
	fb_circle(0x07FF);

	switch (g_state) {
	case ST_RECORDING:
		fb_draw_str_c(28, "REC", 0xF800, 4);
		snprintf(line, sizeof(line), "%d%%", g_ui_pct);
		fb_draw_str_c(112, line, 0xF800, 2);
		fb_bar(g_ui_pct, 0xF800);
		break;
	case ST_PLAYING:
		fb_draw_str_c(28, "PLAY", 0x001F, 3);
		snprintf(line, sizeof(line), "%d%%", g_ui_pct);
		fb_draw_str_c(112, line, 0x001F, 2);
		fb_bar(g_ui_pct, 0x001F);
		break;
	default:
		fb_draw_str_c(36, "REC", 0xF800, 4);			/* S1 */
		fb_draw_str_c(92, "PLAY", g_has_recording ? 0x07E0 : 0x2104, 3);
		break;
	}

	pads_to_hw();
	ui_flush();
}

/* ==================== record path ==================== */

static void adc_dma_finish_isr(void)
{
	rtos_set_semaphore(&adc_sem);
}

static void rec_hw_start(void)
{
	dma_config_t cfg = {0};
	uint32_t fifo = 0;
	aud_adc_config_t adc = DEFAULT_AUD_ADC_CONFIG();

	adc.samp_rate = SAMPLE_RATE;
	adc.clk_src = AUD_CLK_XTAL;		/* production mic config */
	bk_aud_driver_init();
	bk_aud_adc_init(&adc);

	bk_dma_driver_init();
	cfg.mode = DMA_WORK_MODE_REPEAT;
	cfg.chan_prio = 1;
	cfg.src.dev = DMA_DEV_AUDIO_RX;
	cfg.dst.dev = DMA_DEV_DTCM;
	cfg.src.width = DMA_DATA_WIDTH_16BITS;
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
	bk_dma_set_transfer_len(adc_dma, RING_BYTES);
#if (CONFIG_SPE)
	bk_dma_set_dest_sec_attr(adc_dma, DMA_ATTR_SEC);
	bk_dma_set_src_sec_attr(adc_dma, DMA_ATTR_SEC);
#endif
	bk_dma_register_isr(adc_dma, NULL, adc_dma_finish_isr);
	bk_dma_enable_finish_interrupt(adc_dma);

	bk_dma_start(adc_dma);
	bk_aud_adc_start();
}

static void rec_hw_stop(void)
{
	bk_aud_adc_stop();
	bk_dma_stop(adc_dma);
	bk_dma_deinit(adc_dma);
	bk_dma_free(DMA_DEV_AUDIO, adc_dma);
	bk_aud_adc_deinit();
	bk_aud_driver_deinit();
}

static void do_record(void)
{
	uint32_t done = 0;
	int last_pct = -1;
	int timeouts = 0;

	printf("rec: start (%d s, %d bytes)\r\n", REC_SECONDS, REC_BYTES);
	bk_gpio_set_output_high(LED_REC);
	g_ui_pct = 0;
	ui_draw();
	rec_hw_start();

	while (done < REC_WORDS) {
		bk_err_t err = rtos_get_semaphore(&adc_sem, 2000);

		if (err != kNoErr) {
			uint32_t remain = bk_dma_get_remain_len(adc_dma);
			uint32_t status = 0;
			uint32_t f0 = 0, f1 = 0, f2 = 0, f3 = 0;

			bk_aud_adc_get_status(&status);
			bk_aud_adc_get_fifo_data(&f0);
			bk_aud_adc_get_fifo_data(&f1);
			bk_aud_adc_get_fifo_data(&f2);
			bk_aud_adc_get_fifo_data(&f3);
			printf("rec: WAIT timeout (remain=%u st=0x%x fifo=%04x %04x %04x %04x)\r\n",
				remain, status, f0 & 0xffff, f1 & 0xffff, f2 & 0xffff, f3 & 0xffff);
			if (++timeouts >= 3) {
				printf("rec: ABORT (adc dead)\r\n");
				break;
			}
			continue;
		}
		timeouts = 0;

		uint32_t words = REC_WORDS - done;
		if (words > RING_WORDS)
			words = RING_WORDS;
		for (uint32_t i = 0; i < words; i++)
			rec_buf[done + i] = (uint16_t)adc_ring[i];
		done += words;

		int pct = (int)(100ULL * done / REC_WORDS);
		if (pct != last_pct && (pct % 10) == 0) {
			printf("rec: %d%%\r\n", pct);
			g_ui_pct = pct;
			ui_draw();
			last_pct = pct;
		}
	}

	rec_hw_stop();
	bk_gpio_set_output_low(LED_REC);
	if (done >= REC_WORDS) {
		g_has_recording = true;
		printf("rec: done (%u samples)\r\n", done);
	} else {
		printf("rec: failed at %u samples\r\n", done);
	}
}

/* ==================== playback path ==================== */

static void dac_dma_finish_isr(void)
{
	rtos_set_semaphore(&dac_sem);
}

static void play_hw_start(void)
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
	cfg.dst.width = DMA_DATA_WIDTH_16BITS;
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
	bk_dma_set_transfer_len(dac_dma, RING_BYTES);
#if (CONFIG_SPE)
	bk_dma_set_dest_sec_attr(dac_dma, DMA_ATTR_SEC);
	bk_dma_set_src_sec_attr(dac_dma, DMA_ATTR_SEC);
#endif
	bk_dma_register_isr(dac_dma, NULL, dac_dma_finish_isr);
	bk_dma_enable_finish_interrupt(dac_dma);

	bk_dma_start(dac_dma);
	bk_aud_dac_start();
	bk_gpio_set_output_high(PA_MUTE);
}

static void play_hw_stop(void)
{
	bk_gpio_set_output_low(PA_MUTE);
	bk_aud_dac_stop();
	bk_dma_stop(dac_dma);
	bk_dma_deinit(dac_dma);
	bk_dma_free(DMA_DEV_AUDIO, dac_dma);
	bk_aud_dac_deinit();
	bk_aud_driver_deinit();
}

static void do_play(void)
{
	uint32_t done = 0;
	int last_pct = -1;
	int timeouts = 0;

	printf("play: start\r\n");
	bk_gpio_set_output_high(LED_PLAY);
	g_ui_pct = 0;
	ui_draw();

	/* pre-fill the ring, then start */
	for (uint32_t i = 0; i < RING_WORDS; i++)
		dac_ring[i] = (done + i < REC_WORDS) ? rec_buf[done + i] : 0;
	done += RING_WORDS;
	play_hw_start();

	while (done < REC_WORDS) {
		bk_err_t err = rtos_get_semaphore(&dac_sem, 2000);

		if (err != kNoErr) {
			printf("play: WAIT timeout\r\n");
			if (++timeouts >= 3) {
				printf("play: ABORT\r\n");
				break;
			}
			continue;
		}
		timeouts = 0;

		uint32_t words = REC_WORDS - done;
		if (words > RING_WORDS)
			words = RING_WORDS;
		for (uint32_t i = 0; i < words; i++)
			dac_ring[i] = rec_buf[done + i];
		if (words < RING_WORDS)
			memset(&dac_ring[words], 0, (RING_WORDS - words) * 4);
		done += words;

		int pct = (int)(100ULL * done / REC_WORDS);
		if (pct != last_pct && (pct % 10) == 0) {
			printf("play: %d%%\r\n", pct);
			g_ui_pct = pct;
			ui_draw();
			last_pct = pct;
		}
	}

	rtos_delay_milliseconds(300);		/* let the tail drain */
	play_hw_stop();
	bk_gpio_set_output_low(LED_PLAY);
	printf("play: done\r\n");
}

/* ==================== keys + app ==================== */

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

static void key_task(beken_thread_arg_t arg)
{
	(void)arg;
	int rec_hist = 1, play_hist = 1;

	while (1) {
		int rec_now = (bk_gpio_get_input(KEY_REC) == 0);
		int play_now = (bk_gpio_get_input(KEY_PLAY) == 0);

		if (g_state == ST_IDLE) {
			if (rec_now && !rec_hist)
				g_request = 1;
			else if (play_now && !play_hist && g_has_recording)
				g_request = 2;
		}

		rec_hist = rec_now;
		play_hist = play_now;

		if (bk_gpio_get_input(KEY_SPARE) == 0)
			printf("state=%d has_rec=%d\r\n", g_state, g_has_recording);

		rtos_delay_milliseconds(25);
	}
}

static void app_task(beken_thread_arg_t arg)
{
	(void)arg;

	while (1) {
		if (g_request == 1 && g_state == ST_IDLE && rec_buf) {
			g_request = 0;
			g_state = ST_RECORDING;
			do_record();
			g_state = ST_IDLE;
			ui_draw();
		} else if (g_request == 2 && g_state == ST_IDLE && rec_buf && g_has_recording) {
			g_request = 0;
			g_state = ST_PLAYING;
			do_play();
			g_state = ST_IDLE;
			ui_draw();
		}
		rtos_delay_milliseconds(20);
	}
}

static void lcd_backlight_on(void)
{
	pwm_init_config_t cfg = {0};

	bk_pwm_driver_init();
	cfg.period_cycle = PWM_SRC_CLK_HZ / BL_PWM_HZ;
	cfg.duty_cycle   = cfg.period_cycle * BL_DUTY_PCT / 100;
	bk_pwm_init(BL_PWM_CH, &cfg);
	bk_pwm_start(BL_PWM_CH);
}

void user_app_main(void)
{
	printf("\r\n===== rec_play on BK7258 (16k mono, 10 s, PSRAM + LCD) =====\r\n");

	bk_gpio_driver_init();
	gpio_out_init(GPIO_52, 1);		/* panel power */
	lcd_backlight_on();
	gpio_out_init(LED_REC, 0);
	gpio_out_init(LED_PLAY, 0);
	gpio_out_init(PA_MUTE, 0);
	key_in_init(KEY_REC);
	key_in_init(KEY_PLAY);
	key_in_init(KEY_SPARE);

	/* panel gpios + GC9D01 init over the bit-bang */
	gpio_dev_unmap(SCK1);  bk_gpio_disable_pull(SCK1);  bk_gpio_enable_output(SCK1);
	gpio_dev_unmap(CS1);   bk_gpio_disable_pull(CS1);   bk_gpio_enable_output(CS1);
	gpio_dev_unmap(MOSI1); bk_gpio_disable_pull(MOSI1); bk_gpio_enable_output(MOSI1);
	gpio_dev_unmap(DC1);   bk_gpio_disable_pull(DC1);   bk_gpio_enable_output(DC1);
	gpio_dev_unmap(RST1);  bk_gpio_disable_pull(RST1);  bk_gpio_enable_output(RST1);
	gpio_dev_unmap(SCK2);  bk_gpio_disable_pull(SCK2);  bk_gpio_enable_output(SCK2);
	gpio_dev_unmap(CS2);   bk_gpio_disable_pull(CS2);   bk_gpio_enable_output(CS2);
	gpio_dev_unmap(MOSI2); bk_gpio_disable_pull(MOSI2); bk_gpio_enable_output(MOSI2);
	gpio_dev_unmap(DC2);   bk_gpio_disable_pull(DC2);   bk_gpio_enable_output(DC2);
	gpio_dev_unmap(RST2);  bk_gpio_disable_pull(RST2);  bk_gpio_enable_output(RST2);
	FH(RST1); FH(RST2);

	fp_init(&fp_sck1, SCK1);   fp_init(&fp_sck2, SCK2);
	fp_init(&fp_mosi1, MOSI1); fp_init(&fp_mosi2, MOSI2);
	fp_init(&fp_cs1, CS1);     fp_init(&fp_cs2, CS2);
	fp_init(&fp_dc1, DC1);     fp_init(&fp_dc2, DC2);
	P_HI(fp_cs1); P_HI(fp_cs2);

	lcd_init_panels();
	hw_init();
	pads_to_sw();

	rec_buf = psram_zalloc(REC_BYTES);
	printf("rec buf: %p (%u KB) %s\r\n", rec_buf, REC_BYTES / 1024,
		rec_buf ? "ok" : "ALLOC FAILED");

	rtos_init_semaphore_ex(&adc_sem, 1, 0);
	rtos_init_semaphore_ex(&dac_sem, 1, 0);

	g_state = ST_IDLE;
	ui_draw();

	rtos_create_thread(NULL, 4, "keys", key_task, 4096, 0);
	rtos_create_thread(NULL, 5, "app", app_task, 8192, 0);
}

#else /* !CONFIG_SYS_CPU0 */
void user_app_main(void)
{
	printf("rec_play: idle on this core\r\n");
}
#endif

int main(void)
{
	rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
	bk_init();

	return 0;
}
