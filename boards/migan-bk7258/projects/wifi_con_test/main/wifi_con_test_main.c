/*
 * wifi_con_test - simple WiFi station connection test for the migan-bk7258
 *
 * Behavior mirrored from the ESP32-P4 reference
 * (esp32-demo/p4-cb-v3_2/app/wifi_con_test):
 *
 *   1. scan and list the nearby APs (SSID / BSSID / RSSI / channel / security)
 *   2. connect to DEFAULT_AP (30 s timeout)
 *   3. print the acquired IP / gateway / netmask
 *   4. heartbeat-toggle LED1 (P40, high active) every 500 ms and print a
 *      "WiFi status: UP/DOWN" line (RSSI + IP) every 30 s
 *
 * On the BK7258 the WiFi stack is already initialized by bk_init()
 * (bk_event_init + bk_netif_init + bk_wifi_init, gated by
 * CONFIG_WIFI_ENABLE), and the netif layer starts DHCP automatically
 * when the station connects (EVENT_WIFI_STA_CONNECTED). The app only
 * configures the station and listens to the events.
 *
 * Credentials: main/wifi_config.h (copy of wifi_config.h.example,
 * gitignored).
 */

#include <stdio.h>
#include <string.h>

#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <driver/gpio.h>
#include <components/event.h>
#include <components/netif.h>
#include <bk_private/bk_wifi.h>

#include "wifi_config.h"

#ifndef DEFAULT_AP
#error "wifi_config.h missing - copy wifi_config.h.example and fill in your credentials"
#endif

#if (CONFIG_SYS_CPU0)

#define LED0_GPIO		GPIO_40		/* high active on migan */
#define CONNECT_TIMEOUT_MS	30000
#define STATUS_PERIOD_MS	30000

static beken_semaphore_t s_scan_sem;
static beken_semaphore_t s_conn_sem;
static volatile int s_got_ip;

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

/* ---- events ---- */
static int wifi_event_cb(void *arg, event_module_t event_module,
			 int event_id, void *event_data)
{
	(void)arg;
	(void)event_module;

	switch (event_id) {
	case EVENT_WIFI_STA_CONNECTED: {
		wifi_event_sta_connected_t *c = event_data;
		printf("wifi: connected to \"%s\"\r\n", c->ssid);
		break;
	}
	case EVENT_WIFI_STA_DISCONNECTED: {
		wifi_event_sta_disconnected_t *d = event_data;
		printf("wifi: disconnected (reason %d%s)\r\n",
			d->disconnect_reason, d->local_generated ? ", local" : "");
		s_got_ip = 0;
		rtos_set_semaphore(&s_conn_sem);
		break;
	}
	case EVENT_WIFI_SCAN_DONE: {
		wifi_scan_result_t res = {0};

		if (bk_wifi_scan_get_result(&res) == BK_OK) {
			bk_wifi_scan_dump_result(&res);
			bk_wifi_scan_free_result(&res);
		}
		rtos_set_semaphore(&s_scan_sem);
		break;
	}
	default:
		break;
	}

	return BK_OK;
}

static int netif_event_cb(void *arg, event_module_t event_module,
			  int event_id, void *event_data)
{
	netif_event_got_ip4_t *got_ip;

	(void)arg;
	(void)event_module;

	if (event_id == EVENT_NETIF_GOT_IP4) {
		got_ip = (netif_event_got_ip4_t *)event_data;
		printf("netif %d got ip\r\n", got_ip->netif_if);
		s_got_ip = 1;
		rtos_set_semaphore(&s_conn_sem);
	}

	return BK_OK;
}

static void print_ip(void)
{
	netif_ip4_config_t ip4 = {0};

	if (bk_netif_get_ip4_config(NETIF_IF_STA, &ip4) == BK_OK) {
		printf("  IP: %s\r\n  GW: %s\r\n  NM: %s\r\n",
			ip4.ip, ip4.gateway, ip4.mask);
	}
}

/* ---- scan ---- */
static void scan_networks(void)
{
	wifi_scan_config_t scan_cfg = {0};

	printf("--- scanning for available APs ---\r\n");
	rtos_init_semaphore_ex(&s_scan_sem, 2, 0);
	bk_wifi_scan_start(&scan_cfg);

	if (rtos_get_semaphore(&s_scan_sem, 15000) == kNoErr)
		printf("--- scan done ---\r\n");
	else
		printf("--- scan TIMEOUT ---\r\n");
}

/* ---- connect ---- */
static int connect_sta(void)
{
	wifi_sta_config_t sta = {0};
	bk_err_t err;
	int ret = -1;

	strncpy((char *)sta.ssid, DEFAULT_AP, sizeof(sta.ssid) - 1);
	strncpy((char *)sta.password, DEFAULT_PASSWD, sizeof(sta.password) - 1);
	sta.auto_reconnect_count = 0;		/* 0 = always reconnect */
	sta.auto_reconnect_timeout = 0;

	printf("--- connecting to \"%s\" (%d s timeout) ---\r\n",
		DEFAULT_AP, CONNECT_TIMEOUT_MS / 1000);

	s_got_ip = 0;
	err = bk_wifi_sta_set_config(&sta);
	if (err != BK_OK) {
		printf("wifi: set_config failed: %d\r\n", err);
		return -1;
	}
	err = bk_wifi_sta_start();
	if (err != BK_OK) {
		printf("wifi: sta_start failed: %d\r\n", err);
		return -1;
	}

	/* wait for got-ip; ignore intermediate disconnect events
	 * (the stack emits one while (re)associating) */
	for (int i = 0; i < CONNECT_TIMEOUT_MS / 1000 && !s_got_ip; i++)
		rtos_get_semaphore(&s_conn_sem, 1000);

	if (s_got_ip) {
		printf("--- connected successfully ---\r\n");
		print_ip();
		ret = 0;
	} else {
		printf("--- connection TIMEOUT ---\r\n");
	}

	return ret;
}

/* ---- status line every 30 s ---- */
static void print_status(void)
{
	wifi_link_status_t st = {0};

	if (bk_wifi_sta_get_link_status(&st) == BK_OK &&
	    st.state == WIFI_LINKSTATE_STA_GOT_IP) {
		printf("--- WiFi status: UP (\"%s\", RSSI: %d dBm, ch %d) ---\r\n",
			st.ssid, st.rssi, st.channel);
		print_ip();
	} else {
		printf("--- WiFi status: DOWN ---\r\n");
	}
}

void user_app_main(void)
{
	gpio_out_init(LED0_GPIO, 0);

	printf("\r\n========================================\r\n");
	printf("  migan-bk7258  WiFi Connection Test\r\n");
	printf("========================================\r\n");

	bk_event_register_cb(EVENT_MOD_WIFI, EVENT_ID_ALL, wifi_event_cb, NULL);
	bk_event_register_cb(EVENT_MOD_NETIF, EVENT_ID_ALL, netif_event_cb, NULL);

	rtos_init_semaphore_ex(&s_conn_sem, 2, 0);

	scan_networks();

	connect_sta();

	printf("--- entering main loop ---\r\n");

	uint32_t tick = 0;
	while (1) {
		if ((tick % (STATUS_PERIOD_MS / 500)) == 0)
			print_status();

		bk_gpio_set_output_high(LED0_GPIO);
		rtos_delay_milliseconds(250);
		bk_gpio_set_output_low(LED0_GPIO);
		rtos_delay_milliseconds(250);
		tick++;
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
