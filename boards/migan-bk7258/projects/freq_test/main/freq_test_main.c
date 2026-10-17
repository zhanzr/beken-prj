/*
 * freq_test - measure the actual CPU frequency of cp0 and cp1
 *
 * Method: a fixed-instruction-count assembly loop placed in IRAM
 * (section ".iram") - completely independent of the XIP/flash memory
 * path (no flash fetches, registers only). Inner iteration =
 * 16 NOPs + subs + taken-branch ~= 19-20 cycles on a Cortex-M33.
 * The probe counts iterations over a 2 s window and reports
 * kilo-inner-iterations per second. The cp1/cp0 RATE RATIO is the
 * credible core-clock comparison; cp0's 240 MHz config anchors the
 * absolute scale.
 *
 * Flow: cp0 probes, releases cp1 at its per-build generated XIP link
 * address (CONFIG_PRIMARY_CPU1_APP_VIRTUAL_CODE_START from
 * partitions_gen.h - self-adapting to this build's compressed layout),
 * then cp1 probes.
 */

#include <stdio.h>
#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <driver/gpio.h>
#include <driver/pwr_clk.h>
#include <modules/pm.h>
#include "partitions_gen.h"

#ifndef SOC_FLASH_DATA_BASE
#define SOC_FLASH_DATA_BASE	0x02000000
#endif

#define PROBE_MS	2000
#define INNER_NOPS	16
#define INNER_REPS	100
#define CYC_PER_INNER	19	/* 16 NOP + subs + taken branch, approx */

extern void reset_cpu1_core(uint32 offset, uint32_t start_flag);

__attribute__((noinline, section(".iram")))
static uint32_t probe_kilo_iters_per_sec(void)
{
	uint32_t start_tick, now, outer = 0;

	start_tick = (uint32_t)(bk_get_tick() * bk_get_ms_per_tick());

	do {
		for (unsigned r = 0; r < INNER_REPS; r++) {
			__asm volatile(
				"mov r3, %0\n"
				"1:\n"
				"nop\n\tnop\n\tnop\n\tnop\n"
				"nop\n\tnop\n\tnop\n\tnop\n"
				"nop\n\tnop\n\tnop\n\tnop\n"
				"nop\n\tnop\n\tnop\n\tnop\n"
				"subs r3, #1\n"
				"bne 1b\n"
				: : "r" ((unsigned)INNER_NOPS) : "r3", "cc");
		}
		outer += INNER_REPS;
		now = (uint32_t)(bk_get_tick() * bk_get_ms_per_tick());
	} while (now - start_tick < PROBE_MS);

	/* inner iters per second: (outer*100)*1000 / elapsed_ms */
	return (outer * INNER_REPS) * 1000 / (now - start_tick);
}

static void release_cp1(void)
{
	reset_cpu1_core(SOC_FLASH_DATA_BASE + CONFIG_PRIMARY_CPU1_APP_VIRTUAL_CODE_START, 1);
}

#if (CONFIG_SYS_CPU0)

void user_app_main(void)
{
	uint32_t rate;

	bk_pm_cp1_auto_power_down_state_set(0x0);
	bk_pm_module_vote_power_ctrl(PM_POWER_MODULE_NAME_CPU1, PM_POWER_MODULE_STATE_ON);

	printf("\r\n===== freq_test on BK7258 =====\r\n");

	rate = probe_kilo_iters_per_sec();
	printf("cp0: %u kInnerIters/s, ~%u MHz est (config anchor 240 MHz)\r\n",
	       rate, rate * CYC_PER_INNER / 1000);

	release_cp1();

	while (1)
		rtos_delay_milliseconds(5000);
}

#elif (CONFIG_SYS_CPU1)

void user_app_main(void)
{
	/* life sign: 3 blinks on LED2 (P41) */
	bk_gpio_driver_init();
	bk_gpio_enable_output(GPIO_41);
	for (int i = 0; i < 3; i++) {
		bk_gpio_set_output_high(GPIO_41);
		rtos_delay_milliseconds(200);
		bk_gpio_set_output_low(GPIO_41);
		rtos_delay_milliseconds(200);
	}

	rtos_delay_milliseconds(3000);	/* let cp0 print first */

	uint32_t rate = probe_kilo_iters_per_sec();
	printf("cp1: %u kInnerIters/s, ~%u MHz est\r\n",
	       rate, rate * CYC_PER_INNER / 1000);

	while (1)
		rtos_delay_milliseconds(5000);
}

#else

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
