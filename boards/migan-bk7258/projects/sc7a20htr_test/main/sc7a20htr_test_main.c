/*
 * sc7a20htr_test - SC7A20HTR 3-axis g-sensor test for the migan-bk7258
 *
 * The sensor answers on I2C addr 0x18 (SDO strap = low), WHO_AM_I
 * (0x0F) = 0x11, INT1 = P48. The hardware I2C controller (bk_i2c_*,
 * what the vendor bk_gsensor driver uses) phantom-ACKs every address
 * with zero data on this setup, so this app BIT-BANGS the bus with
 * plain GPIO writes (100 kHz, P20 = SCL, P21 = SDA) - which works.
 *
 * Multi-byte auto-increment (0x80|0x28) is also ignored by this part:
 * the six output registers are read one at a time.
 *
 * Behavior:
 *   boot  -> init (50 Hz, XYZ on, 2 G), LED2 heartbeat 1 Hz
 *   every 500 ms -> print X/Y/Z in mg; LED1 (P40) on when moving
 *
 * Hardware (migan schematic, sheet 4): SC7A20HTR, IIC1 bus,
 * SCL = P20, SDA = P21, INT1 = P48, powered from G_VDD (alive).
 */

#include <stdio.h>
#include <string.h>

#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <driver/gpio.h>
#include <driver/i2c.h>

#if (CONFIG_SYS_CPU0)

#define GS_SCL		GPIO_20
#define GS_SDA		GPIO_21
#define GS_ADDR		0x18
#define GS_REG_WHO	0x0F
#define GS_REG_CTRL1	0x20
#define GS_REG_CTRL4	0x23
#define GS_REG_X_L	0x28

#define LED_MOTION	GPIO_40
#define LED_HB		GPIO_41

extern void delay_us(UINT32 us);

/* ---- gpio i2c ---- */
static void bh_delay(void)		{ delay_us(5); }
static void bh_scl(int v)		{ if (v) bk_gpio_set_output_high(GS_SCL); else bk_gpio_set_output_low(GS_SCL); }
static void bh_sda(int v)		{ if (v) bk_gpio_set_output_high(GS_SDA); else bk_gpio_set_output_low(GS_SDA); }
static int  bh_sda_get(void)		{ return bk_gpio_get_input(GS_SDA); }

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

static int bh_write_byte(uint8_t b)	/* returns 0 = ACK */
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

/* one register write */
static int gs_write(uint8_t reg, uint8_t val)
{
	int a;

	bh_start();
	a = bh_write_byte(GS_ADDR << 1);
	bh_write_byte(reg);
	bh_write_byte(val);
	bh_stop();
	return a;
}

/* one register read (no auto-increment - this part ignores 0x80) */
static uint8_t gs_read(uint8_t reg)
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

static void gs_init(void)
{
	int a1, a2;
	uint8_t who = 0;

	/* probe + who_am_i */
	bh_start();
	a1 = bh_write_byte(GS_ADDR << 1);
	a2 = bh_write_byte(GS_REG_WHO);
	bh_start();
	bh_write_byte((GS_ADDR << 1) | 1);
	who = bh_read_byte(0);
	bh_stop();

	printf("gsensor: probe 0x%02x ack=%d%d who_am_i=0x%02x (%s)\r\n",
		GS_ADDR, a1, a2, who,
		(a1 == 0 && who == 0x11) ? "SC7A20 OK" : "NOT RESPONDING");

	if (a1 != 0) {
		printf("gsensor: bus dead - check wiring/power\r\n");
		return;
	}

	gs_write(GS_REG_CTRL4, 0x00);	/* 2 G range, high-res off */
	gs_write(GS_REG_CTRL1, 0x47);	/* 50 Hz, XYZ enable */
	printf("gsensor: configured (50 Hz, 2 G)\r\n");
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

	printf("\r\n===== sc7a20htr_test on BK7258 (bit-bang I2C: SCL=P20 SDA=P21) =====\r\n");

	/* claim the pads EXACTLY like the I2C driver's I2C_SET_PIN does
	 * (map to the I2C0 function + pull-ups + capacity 3) - on this
	 * silicon the GPIO output register still drives the pad, which
	 * is how the bit-bang reaches the sensor */
	gpio_dev_unmap(GS_SCL);
	gpio_dev_unmap(GS_SDA);
	gpio_dev_map(GS_SCL, GPIO_DEV_I2C0_SCL);
	gpio_dev_map(GS_SDA, GPIO_DEV_I2C0_SDA);
	bk_gpio_pull_up(GS_SCL);
	bk_gpio_pull_up(GS_SDA);
	bk_gpio_set_capacity(GS_SCL, 3);
	bk_gpio_set_capacity(GS_SDA, 3);
	bk_gpio_enable_output(GS_SCL);
	bk_gpio_enable_output(GS_SDA);

	/* replay the sequence of the one run where bit-bang worked:
	 * 9-clock recovery -> controller transactions -> bit-bang */
	{
		/* (1) bus recovery: 9 SCL clocks + STOP, pads as GPIO */
		gpio_dev_unmap(GS_SCL);
		gpio_dev_unmap(GS_SDA);
		bk_gpio_disable_pull(GS_SCL);
		bk_gpio_disable_pull(GS_SDA);
		bk_gpio_enable_output(GS_SCL);
		bk_gpio_enable_input(GS_SDA);
		bk_gpio_set_output_high(GS_SCL);
		for (int i = 0; i < 9; i++) {
			bk_gpio_set_output_low(GS_SCL);
			rtos_delay_milliseconds(1);
			bk_gpio_set_output_high(GS_SCL);
			rtos_delay_milliseconds(1);
		}
		bk_gpio_enable_output(GS_SDA);
		bk_gpio_set_output_low(GS_SDA);
		rtos_delay_milliseconds(1);
		bk_gpio_set_output_high(GS_SDA);
		rtos_delay_milliseconds(1);

		/* (2) controller: init + a few transactions (this is the
		 * state the working bit-bang run inherited) */
		i2c_config_t cfg = {0};

		cfg.baud_rate = 100000;
		cfg.addr_mode = I2C_ADDR_MODE_7BIT;
		cfg.slave_addr = GS_ADDR;
		bk_i2c_driver_init();
		bk_i2c_init(I2C_ID_0, &cfg);

		for (uint8_t a = 0x18; a <= 0x19; a++) {
			i2c_mem_param_t p = {0};
			uint8_t dummy = 0;

			p.dev_addr = a;
			p.mem_addr = GS_REG_WHO;
			p.mem_addr_size = 0;
			p.data = &dummy;
			p.data_size = 1;
			p.timeout_ms = 100;
			bk_i2c_memory_read(I2C_ID_0, &p);
		}

		/* (3) restore the pad claim for the bit-bang */
		gpio_dev_unmap(GS_SCL);
		gpio_dev_unmap(GS_SDA);
		gpio_dev_map(GS_SCL, GPIO_DEV_I2C0_SCL);
		gpio_dev_map(GS_SDA, GPIO_DEV_I2C0_SDA);
		bk_gpio_pull_up(GS_SCL);
		bk_gpio_pull_up(GS_SDA);
		bk_gpio_set_capacity(GS_SCL, 3);
		bk_gpio_set_capacity(GS_SDA, 3);
		bk_gpio_enable_output(GS_SCL);
		bk_gpio_enable_output(GS_SDA);
	}

	/* probe with retries - the working run probed at ~7 s after boot */
	for (int attempt = 1; attempt <= 30; attempt++) {
		int a1, a2;
		uint8_t who = 0;

		bh_start();
		a1 = bh_write_byte(GS_ADDR << 1);
		a2 = bh_write_byte(GS_REG_WHO);
		bh_start();
		bh_write_byte((GS_ADDR << 1) | 1);
		who = bh_read_byte(0);
		bh_stop();

		if (a1 == 0 && who == 0x11) {
			printf("gsensor: FOUND after %d probes (%.1f s), who_am_i=0x%02x\r\n",
				attempt, attempt * 0.5f, who);
			break;
		}
		if ((attempt % 10) == 0)
			printf("gsensor: probe %d (t=%.1fs) ack=%d%d who=0x%02x\r\n",
				attempt, attempt * 0.5f, a1, a2, who);
		rtos_delay_milliseconds(500);
	}

	gs_init();
	if (gs_read(GS_REG_WHO) != 0x11) {
		printf("gsensor: still dead, looping LED1\r\n");
		while (1) {
			bk_gpio_set_output_high(LED_MOTION);
			rtos_delay_milliseconds(100);
			bk_gpio_set_output_low(LED_MOTION);
			rtos_delay_milliseconds(400);
		}
	}

	int beat = 0;
	while (1) {
		uint8_t xl = gs_read(GS_REG_X_L + 0);
		uint8_t xh = gs_read(GS_REG_X_L + 1);
		uint8_t yl = gs_read(GS_REG_X_L + 2);
		uint8_t yh = gs_read(GS_REG_X_L + 3);
		uint8_t zl = gs_read(GS_REG_X_L + 4);
		uint8_t zh = gs_read(GS_REG_X_L + 5);

		int mg_x = ((int16_t)((xh << 8) | xl)) / 16;
		int mg_y = ((int16_t)((yh << 8) | yl)) / 16;
		int mg_z = ((int16_t)((zh << 8) | zl)) / 16;
		int moved = (mg_x > 400 || mg_x < -400 ||
			     mg_y > 400 || mg_y < -400 ||
			     mg_z > 1400 || mg_z < 600);

		if (moved)
			bk_gpio_set_output_high(LED_MOTION);
		else
			bk_gpio_set_output_low(LED_MOTION);

		printf("gsensor: x=%5d mg y=%5d mg z=%5d mg %s\r\n",
			mg_x, mg_y, mg_z, moved ? "MOVE" : "");

		rtos_delay_milliseconds(500);

		/* heartbeat: LED2 30 ms blip every second */
		if ((++beat & 1) == 0) {
			bk_gpio_set_output_high(LED_HB);
			rtos_delay_milliseconds(30);
			bk_gpio_set_output_low(LED_HB);
		}
	}
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
