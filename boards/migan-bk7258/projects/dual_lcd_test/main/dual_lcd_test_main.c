/*
 * dual_lcd_test - drive the two GC9D01 SPI LCD modules the beken_genie way
 *
 * Architecture (proven by beken_genie on this board):
 *  - The GC9D01 SPI-LCD driver stack runs in the CP1 firmware
 *    (CONFIG_LCD_SPI_GC9D01=y + DEVICE_NUM=2 in config/bk7258_cp1/config).
 *  - cp0 powers the LDO33 rail (P52) and the backlight (P25 = PWM5 @ 30%),
 *    and boots cp1 via the pm vote (bk_pm_module_vote_boot_cp1_ctrl ON).
 *  - cp1 initializes the LCD driver for both panels (LCD_SPI_ID0/ID1) and
 *    cycles the test patterns through lcd_spi_display_frame().
 *
 * Patterns ported from the ch32v307 nv3030b_md183 test: checkerboard
 * stress, primary solid fills, color bars, vertical gradient.
 * The panels have no touch.
 */

#include <stdio.h>
#include <string.h>
#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <driver/gpio.h>
#include <driver/pwm.h>
#include <driver/pwr_clk.h>
#include <modules/pm.h>
#include <driver/lcd.h>
#include <driver/lcd_spi.h>

#if (CONFIG_SYS_CPU0)

#define BL_PWM_CH	5		/* P25 = PWM5 */
#define BL_PWM_HZ	1000
#define BL_DUTY_PCT	30
#define PWM_SRC_CLK_HZ	26000000

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

void user_app_main(void)
{
	printf("\r\n===== dual_lcd_test on BK7258 =====\r\n");

	/* P52 = LDO33_EN: powers the LCD modules + backlight rail */
	bk_gpio_driver_init();
	gpio_dev_unmap(GPIO_52);
	bk_gpio_disable_pull(GPIO_52);
	bk_gpio_enable_output(GPIO_52);
	bk_gpio_set_output_high(GPIO_52);
	printf("LDO33_EN (P52) high\r\n");

	lcd_backlight_on();

	/* boot cp1: the pm releases it from its partition (the vendor way) */
	bk_pm_module_vote_boot_cp1_ctrl(PM_BOOT_CP1_MODULE_NAME_APP, PM_POWER_MODULE_STATE_ON);
	printf("cp1 boot vote cast\r\n");

	while (1)
		rtos_delay_milliseconds(5000);
}

int main(void)
{
	rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
	bk_init();

	return 0;
}

#elif (CONFIG_SYS_CPU1)

/* ---- cp1: drive both LCD panels ---- */

#include <driver/lcd.h>
#include <driver/lcd_spi.h>

#define LCD_W		160
#define LCD_H		160

static uint16_t fb[LCD_W * LCD_H];

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

static void push_both(void)
{
	lcd_spi_display_frame(0, (uint8_t *)fb, LCD_W, LCD_H);
	lcd_spi_display_frame(1, (uint8_t *)fb, LCD_W, LCD_H);
}

void user_app_main(void)
{
	bk_err_t ret;

	printf("\r\n===== dual_lcd_test patterns on cp1 =====\r\n");

	/* inits BOTH GC9D01 modules (LCD_SPI_ID0 + LCD_SPI_ID1) */
	ret = lcd_driver_init(get_lcd_device_by_name("gc9d01"));
	printf("lcd_driver_init(gc9d01) = %d\r\n", ret);

	while (1) {
		printf("phase: checkerboard stress\r\n");
		fb_checkerboard();
		push_both();
		rtos_delay_milliseconds(3000);

		printf("phase: solid fills (R/G/B/W)\r\n");
		fb_fill(0xF800);  push_both();  rtos_delay_milliseconds(1000);
		fb_fill(0x07E0);  push_both();  rtos_delay_milliseconds(1000);
		fb_fill(0x001F);  push_both();  rtos_delay_milliseconds(1000);
		fb_fill(0xFFFF);  push_both();  rtos_delay_milliseconds(1000);

		printf("phase: color bars\r\n");
		fb_color_bars();
		push_both();
		rtos_delay_milliseconds(3000);

		printf("phase: gradient\r\n");
		fb_gradient();
		push_both();
		rtos_delay_milliseconds(3000);
	}
}

int main(void)
{
	bk_init();

	user_app_main();

	return 0;
}

#elif (CONFIG_SYS_CPU2)

void user_app_main(void)
{
}

int main(void)
{
	bk_init();

	return 0;
}

#endif
