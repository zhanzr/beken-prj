/*
 * dual_GC9D01_160x160 - drive the two GC9D01 SPI LCD modules, alternating the
 * driving method each pass: 'SW' (GPIO bit-bang) => 'HW' (QSPI units,
 * 480M/7) => 'SW' => 'HW' ...
 *
 * Panels (migan schematic):
 *   LCD1: SCLK=P2  CS=P3  MOSI=P4  DC=P5  RST=P45   (QSPI1 unit)
 *   LCD2: SCLK=P22 CS=P23 MOSI=P24 DC=P7  RST=P6    (QSPI0 unit)
 * D/CX is a plain GPIO in both methods (the vendor does the same);
 * the pad mux switches between GPIO mode (SW) and the QSPI function
 * (HW) via the cfg-word second-function bit. HW needs CONFIG_QSPI=y +
 * CONFIG_QSPI_LINE_MODE=1 (3-wire pad map; keeps GPIO_25 = backlight).
 *
 * The panels are ROUND (1.28" 160x160) - pages are laid out for a
 * circular active area (ring + centered text, no corner content).
 *
 * The GC9D01 init sequence is from the vendor driver
 * (bk_peripheral/src/lcd/spi/lcd_spi_gc9d01.c, gc9d01_init_cmds).
 *
 * The backlight: P52 = LDO33_EN (powers the panels + backlight rail),
 * P25 = PWM5 @ 1 kHz 30% (the boost driver needs PWM, not gpio-high).
 */

#include <stdio.h>
#include <string.h>
#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <driver/gpio.h>
#include <driver/pwm.h>
#include <driver/qspi.h>
#include <driver/qspi_types.h>
#include "gpio_ll.h"
#include "qspi_hal.h"
#include "qspi_ll.h"

/* ---- panel pins (migan schematic) ---- */
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

/* ---- backlight ---- */
#define BL_PWM_CH	5		/* P25 = PWM5 */
#define BL_PWM_HZ	1000
#define BL_DUTY_PCT	30
#define PWM_SRC_CLK_HZ	26000000

/* ---- framebuffer: 160x160 RGB565 ---- */
#define LCD_W	160
#define LCD_H	160
static uint16_t fb[LCD_W * LCD_H];

/* ---- gpio helpers ---- */
static gpio_hw_t *const ghw = (gpio_hw_t *)GPIO_LL_REG_BASE;

/* fast output set: direct LL register write (skips the SDK call layers) */
#define FV(g, v)	gpio_ll_set_output_value(ghw, (g), (v))
#define FH(g)		FV((g), 1)
#define FL(g)		FV((g), 0)

/* fastest path: precomputed full cfg words, one volatile store per level
 * (no read-modify-write of the AON register bitfield) */
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

/* ---- HW mode: drive the panels through the QSPI units ----
 * 3-wire (CLK/CSN/IO0) at ~60 MHz (480M PLL / 7), D/CX stays a plain
 * GPIO in both methods. CONFIG_QSPI=y + CONFIG_QSPI_LINE_MODE=1 make
 * bk_qspi_init() map only CSN/CLK/IO0 (IO1 = GPIO_25 stays backlight).
 * Pad switch SW<->HW = cfg word with the second-function bit (6). */
#define CFG_2ND_FUNC	BIT(6)

static qspi_hal_t hw_hal[2];
static bool hw_ok = false;
static uint32_t hw_err;
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

/* send 1..4 bytes: byte0 of `value` goes on the wire first */
static int hw_send(int unit, uint32_t value, uint32_t nbytes)
{
	qspi_hal_t *h = &hw_hal[unit];

	qspi_hal_set_cmd_c_l(h, 0);
	qspi_hal_set_cmd_c_h(h, value);
	qspi_hal_set_cmd_c_cfg1(h, 0x3 << (nbytes * 2));
	qspi_hal_set_cmd_c_cfg2(h, 0);
	qspi_hal_cmd_c_start(h);
	if (hw_wait(unit) != 0) {
		hw_err++;
		if (hw_err == 32)
			printf("HW: unit %d timed out, falling back to SW\r\n", unit);
		return -1;
	}
	return 0;
}

static void hw_dc_lo(void)	{ *fp_dc1.reg = fp_dc1.lo; *fp_dc2.reg = fp_dc2.lo; }
static void hw_dc_hi(void)	{ *fp_dc1.reg = fp_dc1.hi; *fp_dc2.reg = fp_dc2.hi; }

static void hw_push_unit(int unit)
{
	hw_dc_lo();
	hw_send(unit, 0x2A, 1);				/* CASET */
	hw_dc_hi();
	hw_send(unit, (uint32_t)(LCD_W - 1) << 24, 4);
	hw_dc_lo();
	hw_send(unit, 0x2B, 1);				/* RASET */
	hw_dc_hi();
	hw_send(unit, (uint32_t)(LCD_H - 1) << 24, 4);
	hw_dc_lo();
	hw_send(unit, 0x2C, 1);				/* RAMWR */
	hw_dc_hi();

	/* pixel stream, 2 pixels (4 bytes) per op */
	for (uint32_t i = 0; i < LCD_W * LCD_H; i += 2) {
		uint32_t v = (uint32_t)(fb[i] >> 8)
			   | ((uint32_t)(fb[i] & 0xFF) << 8)
			   | ((uint32_t)(fb[i + 1] >> 8) << 16)
			   | ((uint32_t)(fb[i + 1] & 0xFF) << 24);
		if (hw_send(unit, v, 4) != 0 && hw_err >= 32)
			return;
	}
}

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
	cfg.src_clk_div = 7;			/* the vendor LCD_QSPI_60M mode */

	for (int u = 0; u < 2; u++) {
		hw_hal[u].id = (qspi_id_t)u;
		qspi_hal_init(&hw_hal[u]);
		bk_qspi_init((qspi_id_t)u, &cfg);
	}

	/* hw cfg words = idle levels with the pads handed to the QSPI unit */
	hw_w_sck1  = fp_sck1.hi  | CFG_2ND_FUNC;
	hw_w_sck2  = fp_sck2.hi  | CFG_2ND_FUNC;
	hw_w_mosi1 = fp_mosi1.hi | CFG_2ND_FUNC;
	hw_w_mosi2 = fp_mosi2.hi | CFG_2ND_FUNC;
	hw_w_cs1   = fp_cs1.hi   | CFG_2ND_FUNC;
	hw_w_cs2   = fp_cs2.hi   | CFG_2ND_FUNC;

	/* probe both units with one command each */
	hw_err = 0;
	pads_to_hw();
	for (int u = 0; u < 2; u++) {
		*fp_dc1.reg = fp_dc1.lo; *fp_dc2.reg = fp_dc2.lo;
		if (hw_send(u, 0x00, 1) != 0)
			hw_ok = false;
	}
	pads_to_sw();
	if (hw_err == 0) {
		hw_ok = true;
		printf("HW mode ready (2x QSPI, 480M/7)\r\n");
	} else {
		printf("HW probe failed (%u errs), SW only\r\n", hw_err);
	}
}

static void gpio_out(gpio_id_t g)
{
	gpio_dev_unmap(g);
	bk_gpio_disable_pull(g);
	bk_gpio_enable_output(g);
}

/* ---- bit-bang SPI: both panels in lockstep, CS held across a transaction ---- */
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

static void lcd_reset(void)
{
	FL(RST1); FL(RST2);
	rtos_delay_milliseconds(20);
	FH(RST1); FH(RST2);
	rtos_delay_milliseconds(150);
}

/* ---- the GC9D01 init sequence (vendor lcd_spi_gc9d01.c, restructured) ----
 * Each row: {cmd, len, data[len]}; len == 0x80 means: delay data[0] ms.
 * (0xFF is a legitimate data byte here - the unlock cmds send it - so the
 * delay marker must not collide: 0x80 can never be a real data length.) */
#define GC_DELAY	0x80
static const uint8_t gc_init[][34] = {
	{0xFE, 0},
	{0xEF, 0},
	{0x80, 1, 0xFF}, {0x81, 1, 0xFF}, {0x82, 1, 0xFF}, {0x83, 1, 0xFF},
	{0x84, 1, 0xFF}, {0x85, 1, 0xFF}, {0x86, 1, 0xFF}, {0x87, 1, 0xFF},
	{0x88, 1, 0xFF}, {0x89, 1, 0xFF}, {0x8A, 1, 0xFF}, {0x8B, 1, 0xFF},
	{0x8C, 1, 0xFF}, {0x8D, 1, 0xFF}, {0x8E, 1, 0xFF}, {0x8F, 1, 0xFF},
	{0x3A, 1, 0x05},			/* RGB565 */
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
	{0x36, 1, 0x00},			/* MADCTL */
	{0x34, 0},				/* TE off */
	{0x11, 0},				/* sleep out */
	{0x00, GC_DELAY, 120},
	{0x29, 0},				/* display on */
};

static void lcd_init_panels(void)
{
	lcd_reset();

	for (uint32_t i = 0; i < sizeof(gc_init) / sizeof(gc_init[0]); i++) {
		const uint8_t *e = gc_init[i];

		if (e[1] == GC_DELAY) {
			rtos_delay_milliseconds(e[2]);
			continue;
		}
		lcd_cmd_data(e[0], &e[2], e[1]);
	}
}

/* ---- pattern draw helpers ---- */
static void fb_fill(uint16_t c)
{
	for (uint32_t i = 0; i < LCD_W * LCD_H; i++)
		fb[i] = c;
}

static void fb_checkerboard(void)
{
	for (uint32_t y = 0; y < LCD_H; y++)
		for (uint32_t x = 0; x < LCD_W; x++)
			fb[y * LCD_W + x] = ((x ^ y) & 1) ? 0x0000 : 0xFFFF;
}

static void fb_color_bars(void)
{
	static const uint16_t bars[8] = {
		0xF800, 0x07E0, 0x001F, 0xFFE0,
		0xF81F, 0x07FF, 0xFFFF, 0x0000,
	};
	for (uint32_t y = 0; y < LCD_H; y++)
		for (uint32_t x = 0; x < LCD_W; x++)
			fb[y * LCD_W + x] = bars[x * 8 / LCD_W];
}

static void fb_gradient(void)
{
	for (uint32_t y = 0; y < LCD_H; y++) {
		uint16_t c;
		if (y < LCD_H / 3)
			c = (uint16_t)(0x3F - (y * 0x3F) / (LCD_H / 3));
		else if (y < 2 * LCD_H / 3)
			c = (uint16_t)((0x3F * (y - LCD_H / 3)) / (LCD_H / 3)) << 5;
		else
			c = (uint16_t)(0x1F * (y - 2 * LCD_H / 3)) / (LCD_H / 3);
		for (uint32_t x = 0; x < LCD_W; x++)
			fb[y * LCD_W + x] = c;
	}
}

/* ---- 5x7 ASCII font (classic public-domain glcdfont, 0x20..0x7E) ----
 * 5 bytes per glyph, column format, bit0 = top row. */
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

static void fb_px(uint32_t x, uint32_t y, uint16_t c)
{
	if (x < LCD_W && y < LCD_H)
		fb[y * LCD_W + x] = c;
}

/* draw one char, 5x7 glyphs scaled by `scale` (1 px advance = 6*scale) */
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

static void fb_draw_str(uint32_t x, uint32_t y, const char *s, uint16_t color, uint32_t scale)
{
	while (*s) {
		fb_draw_char(x, y, *s++, color, scale);
		x += 6 * scale;
	}
}

/* centered text at scale */
static void fb_draw_str_c(uint32_t y, const char *s, uint16_t color, uint32_t scale)
{
	uint32_t w = (uint32_t)strlen(s) * 6 * scale;

	fb_draw_str((LCD_W > w) ? (LCD_W - w) / 2 : 0, y, s, color, scale);
}

/* the panels are ROUND (1.28" GC9D01): keep content inside the circle -
 * ring border + centered text only, nothing near the corners */
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

static void fb_banner(const char *method, const char *label)
{
	fb_fill(0x0000);
	fb_circle(0x07FF);
	fb_draw_str_c(44, method, 0xF800, 5);		/* SW / HW, big */
	fb_draw_str_c(96, label, 0x07E0, 2);		/* LCD1 / LCD2 */
}

static void fb_info(const char *method, const char *label, uint32_t pass)
{
	char line[16];

	fb_fill(0x0000);
	fb_circle(0x001F);
	snprintf(line, sizeof(line), "%s %s", method, label);
	fb_draw_str_c(44, "GC9D01", 0x07FF, 2);
	fb_draw_str_c(68, line, 0xFFE0, 2);
	snprintf(line, sizeof(line), "PASS %u", pass);
	fb_draw_str_c(96, line, 0xFFFF, 1);
	fb_draw_str_c(108, "BL PWM5", 0x07E0, 1);
	/* RGBW chips centered */
	for (uint32_t i = 0; i < 10; i++) {
		for (uint32_t j = 0; j < 8; j++) {
			fb_px(56 + i, 126 + j, 0xF800);
			fb_px(70 + i, 126 + j, 0x07E0);
			fb_px(84 + i, 126 + j, 0x001F);
			fb_px(98 + i, 126 + j, 0xFFFF);
		}
	}
}

/* ---- push the framebuffer to the panels ----
 * One CS-low session for the whole frame, matching the vendor flow
 * (4-byte CASET/RASET then RAMWR + the pixel stream). */
static void push_stream(void)
{
	tx_mode_cmd();	spi_byte(0x2A);				/* CASET */
	tx_mode_data();	spi_byte(0); spi_byte(0); spi_byte(0); spi_byte(LCD_W - 1);
	tx_mode_cmd();	spi_byte(0x2B);				/* RASET */
	tx_mode_data();	spi_byte(0); spi_byte(0); spi_byte(0); spi_byte(LCD_H - 1);
	tx_mode_cmd();	spi_byte(0x2C);				/* RAMWR */
	tx_mode_data();
	for (uint32_t i = 0; i < LCD_W * LCD_H; i++) {
		spi_byte(fb[i] >> 8);
		spi_byte(fb[i] & 0xFF);
	}
}

/* both panels, identical content, lockstep */
static void push_frame(void)
{
	tx_begin();
	push_stream();
	tx_end();
}

/* one panel only (id 1 or 2); the other panel's CS stays high so it
 * ignores the shared clock/mosi */
static void push_frame_to(uint8_t id)
{
	if (id == 2) P_LO(fp_cs2); else P_LO(fp_cs1);
	push_stream();
	if (id == 2) P_HI(fp_cs2); else P_HI(fp_cs1);
}

/* ---- backlight: P52 LDO33 + P25 PWM5 ---- */

static void lcd_backlight_on(void)
{
	pwm_init_config_t cfg = {0};

	bk_pwm_driver_init();
	cfg.period_cycle = PWM_SRC_CLK_HZ / BL_PWM_HZ;
	cfg.duty_cycle   = cfg.period_cycle * BL_DUTY_PCT / 100;

	bk_pwm_init(BL_PWM_CH, &cfg);
	bk_pwm_start(BL_PWM_CH);
	printf("backlight on (P25=PWM5, %d Hz @ %d%%)\r\n", BL_PWM_HZ, BL_DUTY_PCT);
}

/* push fb to one panel (id 1 or 2) by method; self-heals pads and
 * falls back to SW if the HW units broke */
static void push_page(uint8_t id, bool hw)
{
	if (hw && hw_ok) {
		pads_to_hw();
		hw_push_unit(id == 2 ? 0 : 1);	/* LCD2=QSPI0, LCD1=QSPI1 */
	} else {
		pads_to_sw();
		push_frame_to(id);
	}
	if (hw_err >= 32)
		hw_ok = false;
}

/* push fb to both panels (mirrored) by method */
static void push_both(bool hw)
{
	if (hw && hw_ok) {
		pads_to_hw();
		hw_push_unit(0);
		hw_push_unit(1);
	} else {
		pads_to_sw();
		push_frame();
	}
	if (hw_err >= 32)
		hw_ok = false;
}

void user_app_main(void)
{
#if (CONFIG_SYS_CPU0)
	printf("\r\n===== dual_GC9D01_160x160 on BK7258 (SW-HW alt) =====\r\n");

	/* P52 = LDO33_EN: powers the LCD modules + backlight rail */
	bk_gpio_driver_init();
	gpio_dev_unmap(GPIO_52);
	bk_gpio_disable_pull(GPIO_52);
	bk_gpio_enable_output(GPIO_52);
	bk_gpio_set_output_high(GPIO_52);
	printf("LDO33_EN (P52) high\r\n");

	lcd_backlight_on();

	/* panel gpio: outputs, reset both */
	gpio_out(SCK1);  gpio_out(CS1);  gpio_out(MOSI1);  gpio_out(DC1);  gpio_out(RST1);
	gpio_out(SCK2);  gpio_out(CS2);  gpio_out(MOSI2);  gpio_out(DC2);  gpio_out(RST2);
	FH(RST1); FH(RST2);

	/* snapshot the configured cfg words for the single-store fast path */
	fp_init(&fp_sck1, SCK1);   fp_init(&fp_sck2, SCK2);
	fp_init(&fp_mosi1, MOSI1); fp_init(&fp_mosi2, MOSI2);
	fp_init(&fp_cs1, CS1);     fp_init(&fp_cs2, CS2);
	fp_init(&fp_dc1, DC1);     fp_init(&fp_dc2, DC2);
	P_HI(fp_cs1); P_HI(fp_cs2);		/* CS idle high */
	printf("panel gpios ready (fast-path)\r\n");

	lcd_init_panels();
	printf("panels initialized\r\n");

	hw_init();

	uint32_t pass = 0;
	bool use_hw = false;
	while (1) {
		const char *m = (use_hw && hw_ok) ? "HW" : "SW";

		printf("phase: banner (%s)\r\n", m);
		fb_banner(m, "LCD1");
		push_page(1, use_hw);
		fb_banner(m, "LCD2");
		push_page(2, use_hw);
		rtos_delay_milliseconds(2000);

		printf("phase: checkerboard (%s)\r\n", m);
		fb_checkerboard();
		push_both(use_hw);
		rtos_delay_milliseconds(1000);

		printf("phase: info page (%s)\r\n", m);
		pass++;
		fb_info(m, "LCD1", pass);
		push_page(1, use_hw);
		fb_info(m, "LCD2", pass);
		push_page(2, use_hw);
		rtos_delay_milliseconds(2000);

		printf("phase: solid fills (%s)\r\n", m);
		fb_fill(0xF800);  push_both(use_hw);  rtos_delay_milliseconds(500);
		fb_fill(0x07E0);  push_both(use_hw);  rtos_delay_milliseconds(500);
		fb_fill(0x001F);  push_both(use_hw);  rtos_delay_milliseconds(500);
		fb_fill(0xFFFF);  push_both(use_hw);  rtos_delay_milliseconds(500);

		printf("phase: color bars (%s)\r\n", m);
		fb_color_bars();
		push_both(use_hw);
		rtos_delay_milliseconds(1000);

		printf("phase: gradient (%s)\r\n", m);
		fb_gradient();
		push_both(use_hw);
		rtos_delay_milliseconds(1000);

		use_hw = !use_hw;
	}
#else
	/* only cp0 drives the panels in this test */
	printf("dual_GC9D01_160x160: idle on this core\r\n");
#endif
}

int main(void)
{
	rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
	bk_init();

	return 0;
}
