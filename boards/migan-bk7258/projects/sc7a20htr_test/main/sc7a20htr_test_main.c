/*
 * sc7a20h_test - SC7A20HTR 3-axis g-sensor test for the migan-bk7258
 *
 * Sensor: SC7A20H (Silan, LIS2DH12-compatible), I2C addr 0x18
 * (SDO strapped low via R35), WHO_AM_I = 0x11, INT1 = P48,
 * powered from G_VDD.
 *
 * ROOT CAUSE of the earlier failures: the SC7A20H clock-stretches SCL
 * when busy (datasheet 10.1: the slave pulls SCL low to make the master
 * wait), but i2c_hal_init() arms the controller's SCL timeout with a
 * threshold of 4 - any stretch kills the transaction ("SCL timeout
 * triggered, restart i2c") and leaves the sensor mid-byte, which then
 * ACKs everything with zero data. Fix: disable the SCL timeout after
 * bk_i2c_driver_init (i2c_ll_disable_scl_timeout), soft-reset the
 * sensor (0x68 = 0xA5) and use the proper LIS2DH-style init sequence
 * from the mydazy/esp_sc7a20h reference driver:
 *   CTRL1=0x00, CTRL4=0x80 (HR mode, 2 G), CTRL2=0x01, CTRL5=0x08,
 *   CTRL3=0x40 (AOI1->INT1), CTRL6=0x02, CTRL1=0x47 (50 Hz + XYZ)
 * Data: burst-read 0xA8 x 6, 12-bit left-aligned (>> 4), 1 mg/LSB.
 */

#include <stdio.h>
#include <string.h>

#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <driver/gpio.h>
#include <driver/i2c.h>
#include "i2c_ll.h"

#if (CONFIG_SYS_CPU0)

#define GS_ADDR		0x18
#define GS_REG_WHO	0x0F
#define GS_REG_CTRL1	0x20
#define GS_REG_CTRL2	0x21
#define GS_REG_CTRL3	0x22
#define GS_REG_CTRL4	0x23
#define GS_REG_CTRL5	0x24
#define GS_REG_CTRL6	0x25
#define GS_REG_X_L	0x28
#define GS_REG_SOFT_RST	0x68

#define LED_MOTION	GPIO_40
#define LED_HB		GPIO_41

static i2c_hw_t s_i2chw;

static int gs_write_reg(uint8_t reg, uint8_t val)
{
	i2c_mem_param_t p = {0};

	p.dev_addr = GS_ADDR;
	p.mem_addr = reg;
	p.mem_addr_size = I2C_MEM_ADDR_SIZE_8BIT;
	p.data = &val;
	p.data_size = 1;
	p.timeout_ms = 200;
	return bk_i2c_memory_write(I2C_ID_0, &p);
}

static int gs_read_regs(uint8_t reg, uint8_t *buf, uint32_t len)
{
	i2c_mem_param_t p = {0};

	p.dev_addr = GS_ADDR;
	p.mem_addr = reg;
	p.mem_addr_size = I2C_MEM_ADDR_SIZE_8BIT;
	p.data = buf;
	p.data_size = len;
	p.timeout_ms = 200;
	return bk_i2c_memory_read(I2C_ID_0, &p);
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

void user_app_main(void)
{
	bk_gpio_driver_init();
	gpio_out_init(LED_MOTION, 0);
	gpio_out_init(LED_HB, 0);

	printf("\r\n===== sc7a20h_test on BK7258 (HW I2C0: SCL=P20 SDA=P21) =====\r\n");

	/* bring up I2C0, then disarm the SCL timeout so the sensor's
	 * clock stretching does not abort every transfer. NOTE: the
	 * disable must come AFTER bk_i2c_init - i2c_hal_configure
	 * re-enables the timeout with a threshold of 4! */
	bk_i2c_driver_init();

	i2c_config_t cfg = {0};

	cfg.baud_rate = 100000;
	cfg.addr_mode = I2C_ADDR_MODE_7BIT;
	cfg.slave_addr = GS_ADDR;
	bk_i2c_init(I2C_ID_0, &cfg);

	s_i2chw.i2c0_hw = (i2c_typedef_t *)SOC_I2C0_REG_BASE;
	s_i2chw.i2c1_hw = (i2c_typedef_t *)SOC_I2C1_REG_BASE;
	i2c_ll_disable_scl_timeout(&s_i2chw, I2C_ID_0);
	printf("i2c0 ready, scl timeout disarmed\r\n");

	/* soft reset FIRST (clears any latch left by earlier broken
	 * sessions), then the who_am_i check */
	gs_write_reg(GS_REG_SOFT_RST, 0xA5);
	rtos_delay_milliseconds(20);

	/* who_am_i check */
	uint8_t who = 0;
	int ret = gs_read_regs(GS_REG_WHO, &who, 1);
	printf("gsensor: who_am_i=0x%02x (ret=%d) %s\r\n", who, ret,
		(ret == 0 && who == 0x11) ? "SC7A20 OK" : "NOT RESPONDING");
	int bh_stream(void);

	if (ret != 0 || who != 0x11) {
		printf("gsensor: hw i2c reads wrong - falling back to bit-bang\r\n");
		if (bh_stream() == 0)
			return;		/* bit-bang loop runs forever */
		printf("gsensor: still dead - LED1 fast blink\r\n");
		while (1) {
			bk_gpio_set_output_high(LED_MOTION);
			rtos_delay_milliseconds(100);
			bk_gpio_set_output_low(LED_MOTION);
			rtos_delay_milliseconds(400);
		}
	}

	/* the reference init sequence (mydazy/esp_sc7a20h) */
	gs_write_reg(GS_REG_CTRL1, 0x00);	/* power down before cfg */
	rtos_delay_milliseconds(10);
	gs_write_reg(GS_REG_CTRL4, 0x80);	/* HR mode, 2 G */
	gs_write_reg(GS_REG_CTRL2, 0x01);	/* HPF on for INT */
	gs_write_reg(GS_REG_CTRL5, 0x08);	/* latch INT1 */
	gs_write_reg(GS_REG_CTRL3, 0x40);	/* AOI1 -> INT1 */
	gs_write_reg(GS_REG_CTRL6, 0x02);	/* INT1 active high */
	gs_write_reg(GS_REG_CTRL1, 0x47);	/* 50 Hz + XYZ */
	rtos_delay_milliseconds(10);
	printf("gsensor: configured (HR, 2 G, 50 Hz)\r\n");

	int beat = 0;
	while (1) {
		uint8_t buf[6] = {0};

		if (gs_read_regs(0x80 | GS_REG_X_L, buf, 6) == 0) {
			int mg_x = (((int16_t)((buf[1] << 8) | buf[0])) >> 4);
			int mg_y = (((int16_t)((buf[3] << 8) | buf[2])) >> 4);
			int mg_z = (((int16_t)((buf[5] << 8) | buf[4])) >> 4);
			int moved = (mg_x > 400 || mg_x < -400 ||
				     mg_y > 400 || mg_y < -400 ||
				     mg_z > 1400 || mg_z < 600);

			if (moved)
				bk_gpio_set_output_high(LED_MOTION);
			else
				bk_gpio_set_output_low(LED_MOTION);

			printf("gsensor: x=%5d mg y=%5d mg z=%5d mg %s\r\n",
				mg_x, mg_y, mg_z, moved ? "MOVE" : "");
		}

		rtos_delay_milliseconds(500);

		if ((++beat & 1) == 0) {
			bk_gpio_set_output_high(LED_HB);
			rtos_delay_milliseconds(30);
			bk_gpio_set_output_low(LED_HB);
		}
	}
}

/* ==================== bit-bang fallback (proven path) ============ */
#define BH_SCL	GPIO_20
#define BH_SDA	GPIO_21

extern void delay_us(UINT32 us);

static void bh_delay(void)		{ delay_us(5); }
static void bh_scl(int v)		{ if (v) bk_gpio_set_output_high(BH_SCL); else bk_gpio_set_output_low(BH_SCL); }
static void bh_sda(int v)		{ if (v) bk_gpio_set_output_high(BH_SDA); else bk_gpio_set_output_low(BH_SDA); }
static int  bh_sda_get(void)		{ return bk_gpio_get_input(BH_SDA); }

static void bh_start(void)
{
	bh_sda(1); bh_scl(1); bh_delay();
	bh_sda(0); bh_delay();
	bh_scl(0); bh_delay();
}

static void bh_stop(void)
{
	bh_scl(0); bh_sda(0); bh_delay();
	bh_scl(1); bh_delay();
	bh_sda(1); bh_delay();
}

static int bh_write_byte(uint8_t b)
{
	for (int i = 0; i < 8; i++) {
		bh_sda((b & 0x80) ? 1 : 0);
		b <<= 1;
		bh_delay();
		bh_scl(1); bh_delay();
		bh_scl(0); bh_delay();
	}
	bh_sda(1);
	bh_delay();
	bh_scl(1); bh_delay();
	int ack = bh_sda_get();
	bh_scl(0); bh_delay();
	bh_sda(0);
	return ack;
}

static uint8_t bh_read_byte(int ack)
{
	uint8_t v = 0;

	bh_sda(1);
	for (int i = 0; i < 8; i++) {
		bh_delay();
		bh_scl(1); bh_delay();
		v = (v << 1) | (bh_sda_get() ? 1 : 0);
		bh_scl(0); bh_delay();
	}
	bh_sda(ack ? 0 : 1);
	bh_delay();
	bh_scl(1); bh_delay();
	bh_scl(0); bh_delay();
	bh_sda(1);
	return v;
}

static uint8_t bh_write_reg(uint8_t reg, uint8_t val)
{
	bh_start();
	bh_write_byte(GS_ADDR << 1);
	bh_write_byte(reg);
	bh_write_byte(val);
	bh_stop();
	return 0;
}

static uint8_t bh_read_reg(uint8_t reg)
{
	bh_start();
	bh_write_byte(GS_ADDR << 1);
	bh_write_byte(reg);
	bh_start();
	bh_write_byte((GS_ADDR << 1) | 1);
	uint8_t v = bh_read_byte(0);
	bh_stop();
	return v;
}

int bh_stream(void)
{
	/* pads to GPIO */
	gpio_dev_unmap(BH_SCL);
	gpio_dev_unmap(BH_SDA);
	bk_gpio_disable_pull(BH_SCL);
	bk_gpio_disable_pull(BH_SDA);
	bk_gpio_enable_output(BH_SCL);
	bk_gpio_enable_output(BH_SDA);

	/* 9-clock recovery + STOP */
	bh_scl(1);
	for (int i = 0; i < 9; i++) {
		bh_scl(0); bh_delay();
		bh_scl(1); bh_delay();
	}
	bh_sda(0); bh_delay();
	bh_sda(1); bh_delay();

	/* soft reset + init via bit-bang */
	bh_write_reg(GS_REG_SOFT_RST, 0xA5);
	rtos_delay_milliseconds(20);
	bh_write_reg(GS_REG_CTRL1, 0x00);
	rtos_delay_milliseconds(10);
	bh_write_reg(GS_REG_CTRL4, 0x80);
	bh_write_reg(GS_REG_CTRL2, 0x01);
	bh_write_reg(GS_REG_CTRL5, 0x08);
	bh_write_reg(GS_REG_CTRL3, 0x40);
	bh_write_reg(GS_REG_CTRL6, 0x02);
	bh_write_reg(GS_REG_CTRL1, 0x47);
	rtos_delay_milliseconds(10);

	uint8_t who = bh_read_reg(GS_REG_WHO);
	printf("gsensor[bh]: who_am_i=0x%02x\r\n", who);
	if (who != 0x11) {
		printf("gsensor[bh]: also dead\r\n");
		return -1;
	}
	printf("gsensor[bh]: sensor ALIVE via bit-bang\r\n");

	int beat = 0;
	while (1) {
		uint8_t buf[6] = {0};

		for (int i = 0; i < 6; i++)
			buf[i] = bh_read_reg(GS_REG_X_L + i);

		int mg_x = (((int16_t)((buf[1] << 8) | buf[0])) >> 4);
		int mg_y = (((int16_t)((buf[3] << 8) | buf[2])) >> 4);
		int mg_z = (((int16_t)((buf[5] << 8) | buf[4])) >> 4);
		int moved = (mg_x > 400 || mg_x < -400 ||
			     mg_y > 400 || mg_y < -400 ||
			     mg_z > 1400 || mg_z < 600);

		if (moved)
			bk_gpio_set_output_high(LED_MOTION);
		else
			bk_gpio_set_output_low(LED_MOTION);

		printf("gsensor[bh]: x=%5d mg y=%5d mg z=%5d mg %s\r\n",
			mg_x, mg_y, mg_z, moved ? "MOVE" : "");

		rtos_delay_milliseconds(500);
		if ((++beat & 1) == 0) {
			bk_gpio_set_output_high(LED_HB);
			rtos_delay_milliseconds(30);
			bk_gpio_set_output_low(LED_HB);
		}
	}
	return 0;
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
