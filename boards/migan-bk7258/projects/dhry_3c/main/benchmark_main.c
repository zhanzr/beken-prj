/*
 * 3-core benchmark runner for the BK7258 (migan-bk7258).
 *
 * BK7258 AMP boot model (system_main.c):
 *  - only CPU0 runs user_app_main; cp1/cp2 stay halted until released,
 *  - cp1/cp2 firmware entry is main() (NOT user_app_main).
 *
 * Sequencing (mailbox IPC, cache-coherent):
 *   1. cp0 benches, then GO -> cp1 (CP1_MB_CHNL_IPC)
 *   2. cp1 benches, DONE -> cp0, GO -> cp2 (CP2_MB_CHNL_IPC)
 *   3. cp2 benches (power-voted on by cp0), DONE -> cp0 (CP2_MB_CHNL_USB)
 *
 * All cores run at their maximum frequency: CPU0 votes the system PLL to
 * 480 MHz (bk_pm_module_vote_cpu_freq) -> CPU0 = 240 MHz (its max),
 * CPU1/CPU2 = 480 MHz (their max).
 */

#include <stdio.h>
#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <modules/pm.h>
#include <driver/mailbox_channel.h>
#include <driver/pwr_clk.h>
#include "custom_def.h"

#define BENCH_NAME	"Dhrystone 2.1"

#if (CONFIG_SYS_CPU0)
#define BENCH_CPU_HZ	240000000	/* CPU0 max: 480M PLL / 2 */
#else
#define BENCH_CPU_HZ	480000000	/* CPU1/CPU2 max */
#endif

#define MB_CMD_GO	0x11
#define MB_CMD_DONE	0x12

void dhry_main(uint32_t freq);

static void bench_run(void)
{
	printf("compiler: %s\r\n", COMPILER_NAME);
	printf("flags   : %s\r\n", COMPILER_FLAGS);
	printf("cpu Hz  : %d\r\n", BENCH_CPU_HZ);

	dhry_main(BENCH_CPU_HZ);
}

static void mb_tx_isr(void *param) { (void)param; }
static void mb_tx_cmpl_isr(void *param) { (void)param; }

static void mb_send(unsigned chnl, unsigned cmd)
{
	mb_chnl_cmd_t c = {0};

	c.hdr.cmd = cmd;
	mb_chnl_write(chnl, &c);
}

#if (CONFIG_SYS_CPU0)

static volatile unsigned cp1_done;
static volatile unsigned cp2_done;

static void cp0_rx_isr_cp1(void *param, mb_chnl_cmd_t *cmd_buf)
{
	if (cmd_buf->hdr.cmd == MB_CMD_DONE)
		cp1_done = 1;
}

static void cp0_rx_isr_cp2(void *param, mb_chnl_cmd_t *cmd_buf)
{
	if (cmd_buf->hdr.cmd == MB_CMD_DONE)
		cp2_done = 1;
}

void user_app_main(void)
{
	/* raise the whole system to max: cp0 240M, cp1/cp2 480M */
	bk_pm_module_vote_cpu_freq(PM_DEV_ID_DEFAULT, PM_CPU_FRQ_480M);
	mb_chnl_init();

	mb_chnl_open(CP1_MB_CHNL_IPC, NULL);
	mb_chnl_ctrl(CP1_MB_CHNL_IPC, MB_CHNL_SET_RX_ISR, cp0_rx_isr_cp1);
	mb_chnl_ctrl(CP1_MB_CHNL_IPC, MB_CHNL_SET_TX_ISR, mb_tx_isr);
	mb_chnl_ctrl(CP1_MB_CHNL_IPC, MB_CHNL_SET_TX_CMPL_ISR, mb_tx_cmpl_isr);

	mb_chnl_open(CP2_MB_CHNL_USB, NULL);
	mb_chnl_ctrl(CP2_MB_CHNL_USB, MB_CHNL_SET_RX_ISR, cp0_rx_isr_cp2);
	mb_chnl_ctrl(CP2_MB_CHNL_USB, MB_CHNL_SET_TX_ISR, mb_tx_isr);
	mb_chnl_ctrl(CP2_MB_CHNL_USB, MB_CHNL_SET_TX_CMPL_ISR, mb_tx_cmpl_isr);

	printf("\r\n===== %s on BK7258, cores one-by-one (max freq) =====\r\n", BENCH_NAME);

	bench_run();
	printf(">>> CPU0 done, starting cp1\r\n");

	/* release cp1 at its link address */
	reset_cpu1_core(0x021B0000, 1);
	mb_send(CP1_MB_CHNL_IPC, MB_CMD_GO);

	while (!cp1_done) {
		printf(">>> waiting for cp1...\r\n");
		rtos_delay_milliseconds(5000);
	}
	printf(">>> CPU1 done, starting cp2\r\n");

	/* power cp2's domain, then release it at its link address */
	bk_pm_module_vote_power_ctrl(PM_POWER_MODULE_NAME_CPU2, PM_POWER_MODULE_STATE_ON);
	bk_pm_module_vote_power_ctrl(PM_POWER_MODULE_NAME_CPU2, PM_POWER_MODULE_STATE_ON);
	reset_cpu2_core(0x02220000, 1);
	mb_send(CP2_MB_CHNL_USB, MB_CMD_GO);

	while (!cp2_done) {
		printf(">>> waiting for cp2...\r\n");
		rtos_delay_milliseconds(5000);
	}
	printf(">>> all cores done\r\n");
}

int main(void)
{
	rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
	bk_init();

	return 0;
}

#elif (CONFIG_SYS_CPU1)

/* directed channel values: cp1->cp0 = 0x11, cp1->cp2 = 0x91 */
#define MY_CHNL	0x11	/* cp1 -> cp0 */
#define NEXT_CHNL	0x91	/* cp1 -> cp2 */

static volatile unsigned go;

static void cp1_rx_isr(void *param, mb_chnl_cmd_t *cmd_buf)
{
	if (cmd_buf->hdr.cmd == MB_CMD_GO)
		go = 1;
}

int main(void)
{
	bk_init();
	mb_chnl_init();

	mb_chnl_open(MY_CHNL, NULL);
	mb_chnl_ctrl(MY_CHNL, MB_CHNL_SET_RX_ISR, cp1_rx_isr);
	mb_chnl_ctrl(MY_CHNL, MB_CHNL_SET_TX_ISR, mb_tx_isr);
	mb_chnl_ctrl(MY_CHNL, MB_CHNL_SET_TX_CMPL_ISR, mb_tx_cmpl_isr);

	/* wait for cp0's GO, run anyway after the timeout */
	for (unsigned t = 0; t < 30000 && !go; t += 20)
		rtos_delay_milliseconds(20);
	printf(">>> CPU1 running (go=%d)\r\n", go);

	bench_run();
	printf(">>> CPU1 done, starting cp2\r\n");

	mb_send(NEXT_CHNL, MB_CMD_GO);		/* start cp2 */
	mb_send(MY_CHNL, MB_CMD_DONE);		/* tell cp0 */

	return 0;
}

#elif (CONFIG_SYS_CPU2)

/* directed channel values: cp1->cp2 = 0x91 (rx), cp2->cp0 = 0x21 (tx) */
#define MY_CHNL	0x91	/* cp1 -> cp2 */
#define CP0_CHNL	0x21	/* cp2 -> cp0 */

static volatile unsigned go;

static void cp2_rx_isr(void *param, mb_chnl_cmd_t *cmd_buf)
{
	if (cmd_buf->hdr.cmd == MB_CMD_GO)
		go = 1;
}

int main(void)
{
	bk_init();
	mb_chnl_init();

	mb_chnl_open(CP1_MB_CHNL_IPC, NULL);
	mb_chnl_ctrl(CP1_MB_CHNL_IPC, MB_CHNL_SET_RX_ISR, cp2_rx_isr);
	mb_chnl_ctrl(CP1_MB_CHNL_IPC, MB_CHNL_SET_TX_ISR, mb_tx_isr);
	mb_chnl_ctrl(CP1_MB_CHNL_IPC, MB_CHNL_SET_TX_CMPL_ISR, mb_tx_cmpl_isr);

	/* wait for cp1's GO, run anyway after the timeout */
	for (unsigned t = 0; t < 60000 && !go; t += 20)
		rtos_delay_milliseconds(20);
	printf(">>> CPU2 running (go=%d)\r\n", go);

	bench_run();
	printf(">>> CPU2 done\r\n");

	mb_send(CP0_CHNL, MB_CMD_DONE);

	return 0;
}

#endif
