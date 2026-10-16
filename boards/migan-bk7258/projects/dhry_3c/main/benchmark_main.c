/*
 * 3-core benchmark runner for the BK7258 (migan-bk7258).
 *
 * BK7258 AMP boot model (system_main.c):
 *  - only CPU0 runs user_app_main; cp1/cp2 stay halted until released
 *    (bk_pm_module_vote_boot_cp1_ctrl + start_cpu1_core /
 *     bk_pm_module_vote_power_ctrl + start_cpu2_core),
 *  - cp1/cp2 firmware entry is main() (NOT user_app_main).
 *
 * Sequencing: cp0 runs first, then GOes cp1 over mailbox IPC
 * (CP1_MB_CHNL_IPC), cp1 GOes cp2 (CP2_MB_CHNL_IPC). As a self-recovery
 * fallback, cp1/cp2 run anyway after a timeout if no GO arrives (e.g. when
 * the cores were booted manually via the "bootcore" CLI command).
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
#define MB_CMD_ALLDONE	0x13

#define GO_TIMEOUT_MS	15000		/* run anyway if GO never arrives */

void dhry_main(uint32_t freq);

/* direct slave-core release at the true image addresses (XIP):
 *   cp1 linked at 0x022b0000 = (0x2db000/34)*32 + 0x02000000
 *   cp2 linked at 0x02410000 = (0x451000/34)*32 + 0x02000000
 * (start_cpu1_core()/get_partition_addr() resolve APPLICATION1 to the
 *  AB cpu0-B slot (0x021c0000) on this board - wrong for cp1) */
extern void reset_cpu1_core(uint32 offset, uint32_t start_flag);
extern void reset_cpu2_core(uint32 offset, uint32_t start_flag);

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

static volatile unsigned cp1_all_done;

static void cp0_rx_isr(void *param, mb_chnl_cmd_t *cmd_buf)
{
	if (cmd_buf->hdr.cmd == MB_CMD_ALLDONE)
		cp1_all_done = 1;
}

void user_app_main(void)
{
	/* raise the whole system to max: cp0 240M, cp1/cp2 480M */
	bk_pm_module_vote_cpu_freq(PM_DEV_ID_DEFAULT, PM_CPU_FRQ_480M);

	mb_chnl_open(CP1_MB_CHNL_IPC, NULL);
	mb_chnl_ctrl(CP1_MB_CHNL_IPC, MB_CHNL_SET_RX_ISR, cp0_rx_isr);
	mb_chnl_ctrl(CP1_MB_CHNL_IPC, MB_CHNL_SET_TX_ISR, mb_tx_isr);
	mb_chnl_ctrl(CP1_MB_CHNL_IPC, MB_CHNL_SET_TX_CMPL_ISR, mb_tx_cmpl_isr);

	/* boot the slave cores at their real image addresses */
	reset_cpu1_core(0x022B0000, 1);
	bk_pm_module_vote_power_ctrl(PM_POWER_MODULE_NAME_CPU2, PM_POWER_MODULE_STATE_ON);
	reset_cpu2_core(0x02410000, 1);

	bk_cli_init();

	rtos_delay_milliseconds(200);	/* let cp1/cp2 init their mailboxes */

	printf("\r\n===== %s on BK7258, cores one-by-one (max freq) =====\r\n", BENCH_NAME);

	bench_run();
	printf(">>> CPU0 done, starting cp1\r\n");
	mb_send(CP1_MB_CHNL_IPC, MB_CMD_GO);

	while (!cp1_all_done) {
		printf(">>> waiting for cp1/cp2...\r\n");
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

#define MY_RX_CHNL	CP0_MB_CHNL_IPC
#define MY_TX_CHNL	CP0_MB_CHNL_IPC
#define NEXT_TX_CHNL	CP2_MB_CHNL_IPC

static volatile unsigned go;

static void cp1_rx_isr(void *param, mb_chnl_cmd_t *cmd_buf)
{
	if (cmd_buf->hdr.cmd == MB_CMD_GO)
		go = 1;
}

int main(void)
{
	bk_err_t rc;

	printf("cp1 main entered\r\n");
	bk_init();

	rc = mb_chnl_open(MY_RX_CHNL, NULL);
	printf("cp1: mb_chnl_open(rx) = %d\r\n", rc);
	mb_chnl_ctrl(MY_RX_CHNL, MB_CHNL_SET_RX_ISR, cp1_rx_isr);
	mb_chnl_ctrl(MY_RX_CHNL, MB_CHNL_SET_TX_ISR, mb_tx_isr);
	mb_chnl_ctrl(MY_RX_CHNL, MB_CHNL_SET_TX_CMPL_ISR, mb_tx_cmpl_isr);

	mb_chnl_open(NEXT_TX_CHNL, NULL);
	mb_chnl_ctrl(NEXT_TX_CHNL, MB_CHNL_SET_TX_ISR, mb_tx_isr);
	mb_chnl_ctrl(NEXT_TX_CHNL, MB_CHNL_SET_TX_CMPL_ISR, mb_tx_cmpl_isr);

	/* wait for cp0's GO, run anyway after the timeout */
	for (unsigned t = 0; t < GO_TIMEOUT_MS && !go; t += 20)
		rtos_delay_milliseconds(20);
	printf(">>> CPU1 running (go=%d)\r\n", go);

	bench_run();
	printf(">>> CPU1 done, starting cp2\r\n");

	mb_send(NEXT_TX_CHNL, MB_CMD_GO);		/* start cp2 */
	mb_send(MY_TX_CHNL, MB_CMD_DONE);		/* tell cp0 */

	return 0;
}

#elif (CONFIG_SYS_CPU2)

#define MY_RX_CHNL	CP1_MB_CHNL_IPC

static volatile unsigned go;

static void cp2_rx_isr(void *param, mb_chnl_cmd_t *cmd_buf)
{
	if (cmd_buf->hdr.cmd == MB_CMD_GO)
		go = 1;
}

int main(void)
{
	bk_err_t rc;

	printf("cp2 main entered\r\n");
	bk_init();

	rc = mb_chnl_open(MY_RX_CHNL, NULL);
	printf("cp2: mb_chnl_open(rx) = %d\r\n", rc);
	mb_chnl_ctrl(MY_RX_CHNL, MB_CHNL_SET_RX_ISR, cp2_rx_isr);
	mb_chnl_ctrl(MY_RX_CHNL, MB_CHNL_SET_TX_ISR, mb_tx_isr);
	mb_chnl_ctrl(MY_RX_CHNL, MB_CHNL_SET_TX_CMPL_ISR, mb_tx_cmpl_isr);

	/* wait for cp1's GO, run anyway after the timeout */
	for (unsigned t = 0; t < (2 * GO_TIMEOUT_MS) && !go; t += 20)
		rtos_delay_milliseconds(20);
	printf(">>> CPU2 running (go=%d)\r\n", go);

	bench_run();
	printf(">>> CPU2 done\r\n");

	mb_send(MY_RX_CHNL, MB_CMD_DONE);

	return 0;
}

#endif
