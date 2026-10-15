#include <stdio.h>

#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <driver/pwm.h>

#if (CONFIG_SYS_CPU0)

/*
 * migan-bk7258 PWM test
 *
 *  P25 (GPIO_25 -> PWM5): LCD backlight LED
 *      triangle 0% -> 100% -> 0%, step 1% (continuous "breathing")
 *
 *  P9  (GPIO_9  -> PWM3): onboard motor
 *      triangle 0% -> 100% -> 0%, step 10% (~500 ms active),
 *      then idle (0%) for 1 s, repeat
 *
 * The PWM driver maps the GPIOs to their PWM function itself
 * (GPIO_25 is default-muxed to LCD_G6 - the driver unmaps it).
 * PWM source clock is the 26 MHz XTAL (driver default).
 */

#define BACKLIGHT_PWM_CH	5	/* P25 */
#define BACKLIGHT_FREQ_HZ	1000	/* 1 kHz, no visible flicker */
#define BACKLIGHT_STEP_MS	10	/* full triangle = 200 * 10 ms = 2 s */

#define MOTOR_PWM_CH		3	/* P9 */
#define MOTOR_FREQ_HZ		20000	/* 20 kHz, above audible range */
#define MOTOR_STEP_MS		25	/* 20 steps * 25 ms = 500 ms active */
#define MOTOR_IDLE_MS		1000

#define PWM_SRC_CLK_HZ		26000000

static void pwm_set_duty_percent(pwm_chan_t ch, uint32_t freq_hz, int percent)
{
	pwm_period_duty_config_t cfg = {0};

	cfg.period_cycle = PWM_SRC_CLK_HZ / freq_hz;
	cfg.duty_cycle   = cfg.period_cycle * percent / 100;

	bk_pwm_set_period_duty(ch, &cfg);
}

static void pwm_chan_setup(pwm_chan_t ch, uint32_t freq_hz)
{
	pwm_init_config_t cfg = {0};

	cfg.period_cycle = PWM_SRC_CLK_HZ / freq_hz;
	cfg.duty_cycle   = 0;

	bk_pwm_init(ch, &cfg);
	bk_pwm_start(ch);
}

static beken_thread_t motor_thread_handle;

/* motor: triangle ~500 ms, then 0% idle for 1 s */
static void motor_thread(beken_thread_arg_t arg)
{
	(void)arg;

	pwm_chan_setup(MOTOR_PWM_CH, MOTOR_FREQ_HZ);

	while (1) {
		for (int d = 0; d <= 100; d += 10) {
			pwm_set_duty_percent(MOTOR_PWM_CH, MOTOR_FREQ_HZ, d);
			rtos_delay_milliseconds(MOTOR_STEP_MS);
		}
		for (int d = 90; d >= 0; d -= 10) {
			pwm_set_duty_percent(MOTOR_PWM_CH, MOTOR_FREQ_HZ, d);
			rtos_delay_milliseconds(MOTOR_STEP_MS);
		}

		pwm_set_duty_percent(MOTOR_PWM_CH, MOTOR_FREQ_HZ, 0);
		printf("motor: triangle done, idle %d ms\r\n", MOTOR_IDLE_MS);
		rtos_delay_milliseconds(MOTOR_IDLE_MS);
	}
}

/* user application thread, spawned by the system after bk_init() */
void user_app_main(void)
{
	bk_pwm_driver_init();

	/* backlight LED: continuous triangle, step 1% */
	pwm_chan_setup(BACKLIGHT_PWM_CH, BACKLIGHT_FREQ_HZ);

	printf("pwm_test: P25=PWM5 backlight (1kHz triangle, step 1%%), "
	       "P9=PWM3 motor (20kHz triangle step 10%% + 1s idle)\r\n");

	rtos_create_thread(&motor_thread_handle, 7, "motor_pwm",
			   motor_thread, 4096, 0);

	int count = 0;

	while (1) {
		for (int d = 0; d <= 100; d++) {
			pwm_set_duty_percent(BACKLIGHT_PWM_CH, BACKLIGHT_FREQ_HZ, d);
			rtos_delay_milliseconds(BACKLIGHT_STEP_MS);
		}
		for (int d = 99; d >= 0; d--) {
			pwm_set_duty_percent(BACKLIGHT_PWM_CH, BACKLIGHT_FREQ_HZ, d);
			rtos_delay_milliseconds(BACKLIGHT_STEP_MS);
		}
		printf("backlight triangle %d done\r\n", ++count);
	}
}

#else /* CONFIG_SYS_CPU1 / CONFIG_SYS_CPU2: keep slave cores idle */

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
