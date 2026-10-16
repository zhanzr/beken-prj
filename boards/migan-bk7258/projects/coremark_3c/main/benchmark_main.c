/*
 * 3-core benchmark runner for the BK7258 (migan-bk7258).
 *
 * BK7258 AMP boot model (system_main.c):
 *  - only CPU0 runs user_app_main; cp1/cp2 stay halted until
 *    start_cpu1_core() / start_cpu2_core() are called,
 *  - cp1/cp2 firmware entry is main() (NOT user_app_main).
 *
 * Turn-taking via vendor mailbox IPC (cache-coherent):
 *   cp0 --(CP1_MB_CHNL_IPC)--> cp1 --(CP2_MB_CHNL_IPC)--> cp2
 * acks flow back cp2 -> cp1 -> cp0.
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
#include "utils.h"
#include "core_portme.h"

#define BENCH_NAME	"CoreMark 1.0.1"

#if (CONFIG_SYS_CPU0)
#define BENCH_CPU_HZ	240000000	/* CPU0 max: 480M PLL / 2 */
#else
#define BENCH_CPU_HZ	480000000	/* CPU1/CPU2 max */
#endif

#define MB_CMD_GO	0x11
#define MB_CMD_DONE	0x12
#define MB_CMD_ALLDONE	0x13

int coremark_main(void);

static void bench_run(void)
{
	printf("compiler: %s\r\n", COMPILER_VERSION);
	printf("flags   : %s\r\n", COMPILER_FLAGS);
	printf("cpu Hz  : %d\r\n", BENCH_CPU_HZ);

	coremark_main();
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

	/* boot the slave cores (they halt until released) */
	start_cpu1_core();
	start_cpu2_core();
	rtos_delay_milliseconds(100);	/* let cp1/cp2 init their mailboxes */

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

static volatile unsigned go;
static volatile unsigned cp2_done;

static void cp1_rx_isr(void *param, mb_chnl_cmd_t *cmd_buf)
{
	switch (cmd_buf->hdr.cmd) {
	case MB_CMD_GO:		go = 1; break;
	case MB_CMD_ALLDONE:	cp2_done = 1; break;
	}
}

int main(void)
{
	bk_init();

	mb_chnl_open(CP0_MB_CHNL_IPC, NULL);
	mb_chnl_ctrl(CP0_MB_CHNL_IPC, MB_CHNL_SET_RX_ISR, cp1_rx_isr);
	mb_chnl_ctrl(CP0_MB_CHNL_IPC, MB_CHNL_SET_TX_ISR, mb_tx_isr);
	mb_chnl_ctrl(CP0_MB_CHNL_IPC, MB_CHNL_SET_TX_CMPL_ISR, mb_tx_cmpl_isr);

	mb_chnl_open(CP2_MB_CHNL_IPC, NULL);
	mb_chnl_ctrl(CP2_MB_CHNL_IPC, MB_CHNL_SET_RX_ISR, cp1_rx_isr);
	mb_chnl_ctrl(CP2_MB_CHNL_IPC, MB_CHNL_SET_TX_ISR, mb_tx_isr);
	mb_chnl_ctrl(CP2_MB_CHNL_IPC, MB_CHNL_SET_TX_CMPL_ISR, mb_tx_cmpl_isr);

	printf("cp1 ready, waiting for GO\r\n");

	while (!go)
		rtos_delay_milliseconds(20);

	printf(">>> CPU1 running\r\n");
	bench_run();
	printf(">>> CPU1 done, starting cp2\r\n");

	mb_send(CP2_MB_CHNL_IPC, MB_CMD_GO);		/* start cp2 */
	mb_send(CP0_MB_CHNL_IPC, MB_CMD_DONE);		/* tell cp0 */

	while (!cp2_done)
		rtos_delay_milliseconds(20);

	mb_send(CP0_MB_CHNL_IPC, MB_CMD_ALLDONE);	/* all finished */

	return 0;
}

#elif (CONFIG_SYS_CPU2)

static volatile unsigned go;

static void cp2_rx_isr(void *param, mb_chnl_cmd_t *cmd_buf)
{
	if (cmd_buf->hdr.cmd == MB_CMD_GO)
		go = 1;
}

int main(void)
{
	bk_init();

	mb_chnl_open(CP1_MB_CHNL_IPC, NULL);
	mb_chnl_ctrl(CP1_MB_CHNL_IPC, MB_CHNL_SET_RX_ISR, cp2_rx_isr);
	mb_chnl_ctrl(CP1_MB_CHNL_IPC, MB_CHNL_SET_TX_ISR, mb_tx_isr);
	mb_chnl_ctrl(CP1_MB_CHNL_IPC, MB_CHNL_SET_TX_CMPL_ISR, mb_tx_cmpl_isr);

	printf("cp2 ready, waiting for GO\r\n");

	while (!go)
		rtos_delay_milliseconds(20);

	printf(">>> CPU2 running\r\n");
	bench_run();
	printf(">>> CPU2 done\r\n");

	mb_send(CP1_MB_CHNL_IPC, MB_CMD_DONE);

	return 0;
}

#endif
