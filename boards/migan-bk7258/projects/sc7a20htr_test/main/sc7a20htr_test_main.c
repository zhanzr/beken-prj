/*
 * sc7a20htr_test - exercise the board's SC7A20HTR 3-axis g-sensor
 *
 * Hardware (migan schematic, sheet 4):
 *   SC7A20HTR on I2C0: SCL = P20, SDA = P21 (7-bit addr 0x18)
 *   INT1 -> P48 (falling edge, drive by the vendor driver)
 *   I2C address straps: SDO via R35; the vendor driver uses 0x18.
 *
 * Uses the vendor bk_gsensor component (components/bk_gsensor):
 *   bk_gsensor_init("sc7a20") -> open -> 50 Hz / 2 G / normal mode ->
 *   callback delivers XYZ batches; this app prints them as mg and
 *   lights LED1 (P40) on motion.
 *
 *   LED2 (P41) = heartbeat, blinks every second (I2C/system alive).
 */

#include <stdio.h>
#include <string.h>

#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <driver/gpio.h>
#include <driver/i2c.h>
#include <components/bk_gsensor.h>
#include <driver/pwr_clk.h>

#if (CONFIG_SYS_CPU0)

#define LED1_GPIO	GPIO_40		/* motion */
#define LED2_GPIO	GPIO_41		/* heartbeat */
#define G_RANGE_LSB_MG	16		/* 2 G range: 16 LSB per mg */

static volatile gsensor_xyz_t s_last;
static volatile uint32_t s_samples;
static volatile bool s_alive;

static void gs_cb(void *handle, gsensor_data_t *data)
{
	(void)handle;

	if (data == NULL || data->count == 0)
		return;

	for (unsigned int i = 0; i < data->count; i++) {
		s_last = data->xyz[i];
		s_samples++;
	}
	s_alive = true;
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

/* read WHO_AM_I (0x0F) at both SDO strap addresses */
static void i2c_probe(void)
{
	i2c_config_t cfg = {0};

	cfg.baud_rate = 100000;
	cfg.addr_mode = I2C_ADDR_MODE_7BIT;
	cfg.slave_addr = 0x18;
	bk_i2c_driver_init();
	bk_i2c_init(I2C_ID_0, &cfg);

	/* full address sweep: who answers? */
	printf("i2c sweep:");
	for (uint8_t addr = 0x08; addr < 0x78; addr++) {
		i2c_mem_param_t p = {0};
		uint8_t id = 0;

		p.dev_addr = addr;
		p.mem_addr = 0x0F;
		p.mem_addr_size = 0;
		p.data = &id;
		p.data_size = 1;
		p.timeout_ms = 60;
		if (bk_i2c_memory_read(I2C_ID_0, &p) == 0)
			printf(" 0x%02x(id=%02x)", addr, id);
	}
	printf("\r\n");
}
/* a slave latched mid-transaction (e.g. reset during a prior session)
 * holds SDA low and ACKs everything with zero data - clock it out */
static void i2c_bus_recover(void)
{
	int scl = bk_gpio_get_input(GPIO_20);
	int sda = bk_gpio_get_input(GPIO_21);

	printf("i2c idle levels (pad default): scl=%d sda=%d\r\n", scl, sda);

	/* kill the internal pulls, then re-read: a real bus has EXTERNAL
	 * 4.7K pull-ups and stays high; a floating bus reads 0 */
	gpio_dev_unmap(GPIO_20);
	gpio_dev_unmap(GPIO_21);
	bk_gpio_disable_pull(GPIO_20);
	bk_gpio_disable_pull(GPIO_21);
	bk_gpio_enable_input(GPIO_20);
	bk_gpio_enable_input(GPIO_21);
	rtos_delay_milliseconds(2);
	scl = bk_gpio_get_input(GPIO_20);
	sda = bk_gpio_get_input(GPIO_21);
	printf("i2c idle levels (no pull): scl=%d sda=%d %s\r\n", scl, sda,
		(scl && sda) ? "(external pull-ups ok)" :
		"(!FLOATING - no external pull-up / sensor absent!)");

	/* always clock out 9 pulses + STOP: a slave latched mid-write
	 * ACKs every byte (idle SDA can still be high) */

	gpio_dev_unmap(GPIO_20);
	gpio_dev_unmap(GPIO_21);
	bk_gpio_disable_pull(GPIO_20);
	bk_gpio_disable_pull(GPIO_21);
	bk_gpio_enable_output(GPIO_20);
	bk_gpio_enable_input(GPIO_21);		/* release SDA */
	bk_gpio_set_output_high(GPIO_20);

	for (int i = 0; i < 9; i++) {
		bk_gpio_set_output_low(GPIO_20);
		rtos_delay_milliseconds(1);
		bk_gpio_set_output_high(GPIO_20);
		rtos_delay_milliseconds(1);
	}

	/* STOP condition: SDA low->high while SCL high */
	bk_gpio_enable_output(GPIO_21);
	bk_gpio_set_output_low(GPIO_21);
	rtos_delay_milliseconds(1);
	bk_gpio_set_output_high(GPIO_21);
	rtos_delay_milliseconds(1);

	printf("i2c: bus recovered (sda now %d)\r\n", bk_gpio_get_input(GPIO_21));

	/* G_VDD detector: R44 pulls INT1 (P48) up to G_VDD. With the
	 * internal pull-DOWN active, P48 high = G_VDD alive, P48 low =
	 * G_VDD dead (sensor + its pull-ups unpowered!). */
	bk_gpio_enable_pull(GPIO_48);
	bk_gpio_pull_down(GPIO_48);
	bk_gpio_enable_input(GPIO_48);
	rtos_delay_milliseconds(2);
	printf("g_vdd detect: P48 with pull-down = %d -> G_VDD %s\r\n",
		bk_gpio_get_input(GPIO_48),
		bk_gpio_get_input(GPIO_48) ? "ALIVE" : "DEAD (R37/rail missing?)");
	bk_gpio_disable_pull(GPIO_48);
}

void user_app_main(void)
{
	bk_gpio_driver_init();
	gpio_out_init(LED1_GPIO, 0);
	gpio_out_init(LED2_GPIO, 0);

	printf("\r\n===== sc7a20htr_test on BK7258 (I2C0: SCL=P20 SDA=P21, INT1=P48) =====\r\n");

	bk_gpio_driver_init();

	/* I2C0 lives in the BAKP power domain: hold it on (the vendor
	 * driver only votes with CONFIG_I2C_PM_CB_SUPPORT) */
	bk_pm_module_vote_power_ctrl(PM_POWER_SUB_MODULE_NAME_BAKP_I2C0, PM_POWER_MODULE_STATE_ON);

	i2c_bus_recover();
	i2c_probe();

	void *h = bk_gsensor_init("sc7a20");
	if (h == NULL) {
		printf("gsensor: init FAILED (check I2C0 wiring / addr)\r\n");
		while (1) {
			bk_gpio_set_output_high(LED1_GPIO);
			rtos_delay_milliseconds(100);
			bk_gpio_set_output_low(LED1_GPIO);
			rtos_delay_milliseconds(400);
		}
	}

	bk_gsensor_registerCallback(h, gs_cb);
	bk_gsensor_open(h);
	bk_gsensor_setDatarate(h, GSENSOR_DR_50HZ);
	bk_gsensor_setDateRange(h, GSENSOR_RANGE_2G);
	bk_gsensor_setMode(h, GSENSOR_MODE_NOMAL);
	printf("gsensor: sc7a20 opened (50 Hz, 2 G, normal mode)\r\n");

	uint32_t last_printed = 0;
	int beat = 0;

	while (1) {
		rtos_delay_milliseconds(1000);
		bk_gpio_set_output_high(LED2_GPIO);
		rtos_delay_milliseconds(30);
		bk_gpio_set_output_low(LED2_GPIO);

		uint32_t n = s_samples;

		if (n == last_printed && !s_alive) {
			printf("gsensor: no data yet (samples=%u)\r\n", n);
			continue;
		}
		last_printed = n;

		int mg_x = s_last.x / G_RANGE_LSB_MG;
		int mg_y = s_last.y / G_RANGE_LSB_MG;
		int mg_z = s_last.z / G_RANGE_LSB_MG;
		int moved = (mg_x > 400 || mg_x < -400 ||
			     mg_y > 400 || mg_y < -400 ||
			     mg_z > 1400 || mg_z < 600);

		printf("gsensor: x=%5d mg y=%5d mg z=%5d mg samples=%u %s\r\n",
			mg_x, mg_y, mg_z, n, moved ? "MOVE" : "");
		bk_gpio_set_output_high(LED1_GPIO);
		if (moved)
			bk_gpio_set_output_high(LED1_GPIO);
		else
			bk_gpio_set_output_low(LED1_GPIO);
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
