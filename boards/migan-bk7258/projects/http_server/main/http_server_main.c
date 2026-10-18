/*
 * http_server - BK7258 embedded web server demo
 *
 * Port of the ESP32 e_server / board_server demo
 * (esp32-demo/p4-cb-v3_2) to the migan-bk7258. The frontend is the
 * copied + adapted e_server single page (e_server/web), bundled into
 * main/web_assets.h by e_server/build_web.py.
 *
 * Endpoints (same JSON contract as the reference):
 *   GET  /                 gzip single-page app (web_assets.h)
 *   GET/POST /api/leds     {"leds":[led1,led2]} - LED1 = P40, LED2 = P41
 *   GET  /api/adc          {"temp_c":..,"vbat_v":..} - die temp + VBAT
 *   GET  /api/camera       {"source":"gc2145","ready":..,"w":..,"h":..,"frames":..}
 *   GET  /api/camera/control?rot=&quality=  -> {"rot":..,"quality":..}
 *   GET  /api/info         {"arch":"bk7258","lan_ip":..,"public_ip":null,...}
 *   GET  /stream           MJPEG (camera pipeline not wired yet -> 503)
 *   GET  /capture          single JPEG (ditto -> 503)
 *   GET  /public/<name>    embedded images (web_assets.h table)
 *
 * The g-sensor is left out on purpose (not operational on this board).
 * The camera presence is probed at boot over the camera I2C bus
 * (P42 = SCL, P43 = SDA, GC2145 at 0x3C/0x3D).
 *
 * The HTTP server is a small lwIP-socket implementation (one thread,
 * one request per connection, Connection: close).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <driver/gpio.h>
#include <components/event.h>
#include <components/netif.h>
#include <bk_private/bk_wifi.h>
#include "lwip/sockets.h"
#include <temp_detect_pub.h>

#include "web_assets.h"
#include "wifi_config.h"

#ifndef DEFAULT_AP
#error "wifi_config.h missing - copy wifi_config.h.example and fill in your credentials"
#endif

#if (CONFIG_SYS_CPU0)

#define LED1_GPIO	GPIO_40
#define LED2_GPIO	GPIO_41

#define CAM_SCL		GPIO_42
#define CAM_SDA		GPIO_43

#define HTTP_PORT	80
#define REQ_MAX		2048

extern void delay_us(UINT32 us);

static int s_led[2];
static char s_lan_ip[16];
static volatile int s_got_ip;
static int s_cam_ready;
static beken_semaphore_t s_conn_sem;

/* ==================== GPIO / I2C bit-bang (camera probe) ============ */
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

static void bh_delay(void)		{ delay_us(5); }
static void bh_scl(int v)		{ if (v) bk_gpio_set_output_high(CAM_SCL); else bk_gpio_set_output_low(CAM_SCL); }
static void bh_sda(int v)		{ if (v) bk_gpio_set_output_high(CAM_SDA); else bk_gpio_set_output_low(CAM_SDA); }
static int  bh_sda_get(void)		{ return bk_gpio_get_input(CAM_SDA); }

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

static int cam_probe_addr(uint8_t addr)
{
	int ack;

	bh_start();
	ack = bh_write_byte(addr << 1);
	bh_stop();
	return ack == 0;
}

static void camera_probe(void)
{
	gpio_dev_unmap(CAM_SCL);
	gpio_dev_unmap(CAM_SDA);
	bk_gpio_disable_pull(CAM_SCL);
	bk_gpio_disable_pull(CAM_SDA);
	bk_gpio_enable_output(CAM_SCL);
	bk_gpio_enable_output(CAM_SDA);

	s_cam_ready = cam_probe_addr(0x3C) || cam_probe_addr(0x3D);
	printf("camera: GC2145 %s (I2C1 P42/P43 probe 0x3C/0x3D)\r\n",
		s_cam_ready ? "detected" : "not detected");
}

/* ==================== WiFi ==================== */
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
	default:
		break;
	}
	return BK_OK;
}

static int netif_event_cb(void *arg, event_module_t event_module,
			  int event_id, void *event_data)
{
	(void)arg;
	(void)event_module;
	(void)event_data;

	if (event_id == EVENT_NETIF_GOT_IP4) {
		netif_ip4_config_t ip4 = {0};

		if (bk_netif_get_ip4_config(NETIF_IF_STA, &ip4) == BK_OK) {
			strncpy(s_lan_ip, ip4.ip, sizeof(s_lan_ip) - 1);
			printf("netif: ip %s gw %s\r\n", ip4.ip, ip4.gateway);
		}
		s_got_ip = 1;
		rtos_set_semaphore(&s_conn_sem);
	}
	return BK_OK;
}

static int wifi_connect(void)
{
	wifi_sta_config_t sta = {0};

	bk_event_register_cb(EVENT_MOD_WIFI, EVENT_ID_ALL, wifi_event_cb, NULL);
	bk_event_register_cb(EVENT_MOD_NETIF, EVENT_ID_ALL, netif_event_cb, NULL);

	strncpy((char *)sta.ssid, DEFAULT_AP, sizeof(sta.ssid) - 1);
	strncpy((char *)sta.password, DEFAULT_PASSWD, sizeof(sta.password) - 1);
	sta.auto_reconnect_count = 0;
	sta.auto_reconnect_timeout = 0;

	if (bk_wifi_sta_set_config(&sta) != BK_OK ||
	    bk_wifi_sta_start() != BK_OK) {
		printf("wifi: start failed\r\n");
		return -1;
	}

	for (int i = 0; i < 30 && !s_got_ip; i++)
		rtos_get_semaphore(&s_conn_sem, 1000);

	return s_got_ip ? 0 : -1;
}

/* ==================== tiny HTTP server ==================== */
static int send_all(int fd, const void *buf, int len)
{
	const char *p = buf;
	int sent = 0;

	while (sent < len) {
		int n = send(fd, p + sent, len - sent, 0);
		if (n <= 0)
			return -1;
		sent += n;
	}
	return 0;
}

static void send_reply(int fd, int code, const char *ctype,
		       const char *extra, const void *body, int len)
{
	char hdr[256];
	int n = snprintf(hdr, sizeof(hdr),
			 "HTTP/1.1 %d %s\r\n"
			 "Content-Type: %s\r\n"
			 "Content-Length: %d\r\n"
			 "Connection: close\r\n"
			 "Cache-Control: no-store\r\n"
			 "%s\r\n",
			 code, code == 200 ? "OK" : "Service Unavailable",
			 ctype, len, extra ? extra : "");

	send_all(fd, hdr, n);
	if (len > 0)
		send_all(fd, body, len);
}

static void send_json_str(int fd, const char *json)
{
	send_reply(fd, 200, "application/json", NULL, json, strlen(json));
}

/* parse [a,b] after "leds" */
static int parse_leds(const char *body, int *l1, int *l2)
{
	const char *p = strstr(body, "leds");
	char *end;

	if (!p)
		return -1;
	p = strchr(p, '[');
	if (!p)
		return -1;
	p++;
	*l1 = (int)strtol(p, &end, 10);
	if (end == p)
		return -1;
	p = strchr(end, ',');
	if (!p)
		return -1;
	p++;
	*l2 = (int)strtol(p, &end, 10);
	return (end == p) ? -1 : 0;
}

static void apply_leds(void)
{
	if (s_led[0])
		bk_gpio_set_output_high(LED1_GPIO);
	else
		bk_gpio_set_output_low(LED1_GPIO);
	if (s_led[1])
		bk_gpio_set_output_high(LED2_GPIO);
	else
		bk_gpio_set_output_low(LED2_GPIO);
}

static void handle_client(int fd)
{
	static char req[REQ_MAX + 1];
	static char resp[512];
	int total = 0;
	char *body = NULL;

	/* read headers + full body (they may arrive in separate segments) */
	while (total < REQ_MAX) {
		int n = recv(fd, req + total, REQ_MAX - total, 0);

		if (n <= 0)
			break;
		total += n;
		req[total] = 0;

		char *he = strstr(req, "\r\n\r\n");
		if (he) {
			int clen = 0;
			char *cl = strstr(req, "Content-Length:");
			if (cl)
				clen = atoi(cl + 15);
			if ((total - (int)(he + 4 - req)) >= clen) {
				body = he + 4;
				break;
			}
		}
	}
	if (total <= 0) {
		close(fd);
		return;
	}
	req[total] = 0;

	char *method = req;
	char *path = strchr(req, ' ');
	if (!path) {
		close(fd);
		return;
	}
	*path++ = 0;
	char *sp = strchr(path, ' ');
	if (sp)
		*sp = 0;
	char *q = strchr(path, '?');
	if (q)
		*q++ = 0;

	/* ---- routes ---- */
	if (strcmp(path, "/") == 0) {
		send_reply(fd, 200, "text/html; charset=utf-8",
			   "Content-Encoding: gzip\r\n",
			   index_html_gz, sizeof(index_html_gz));
	} else if (strcmp(path, "/api/leds") == 0) {
		if (strcmp(method, "POST") == 0 && body) {
			int l1, l2;

			if (parse_leds(body, &l1, &l2) == 0) {
				s_led[0] = !!l1;
				s_led[1] = !!l2;
				apply_leds();
			}
		}
		snprintf(resp, sizeof(resp), "{\"leds\":[%d,%d]}",
			 s_led[0], s_led[1]);
		send_json_str(fd, resp);
	} else if (strcmp(path, "/api/adc") == 0) {
		uint32_t t = 0, mv = 0;
		int ok_t = temp_detect_get_temperature(&t) == 0;
		int ok_v = volt_single_get_current_voltage(&mv) == 0;
		/* raw die-temp value -> tenths of degC (observed 550 @ ~55 C) */
		int tc10 = ok_t ? (int)t : -9990;
		int mv_i = ok_v ? (int)mv : 0;

		snprintf(resp, sizeof(resp),
			 "{\"temp_c\":%d.%d,\"vbat_v\":%d.%03d}",
			 tc10 / 10, tc10 % 10, mv_i / 1000, mv_i % 1000);
		send_json_str(fd, resp);
	} else if (strcmp(path, "/api/camera") == 0) {
		snprintf(resp, sizeof(resp),
			 "{\"source\":\"gc2145\",\"ready\":%d,\"w\":0,\"h\":0,\"frames\":0}",
			 s_cam_ready ? 1 : 0);
		send_json_str(fd, resp);
	} else if (strcmp(path, "/api/camera/control") == 0) {
		int rot = -1, quality = -1;

		if (q) {
			char *r = strstr(q, "rot=");
			char *qq = strstr(q, "quality=");
			if (r)
				rot = atoi(r + 4);
			if (qq)
				quality = atoi(qq + 8);
		}
		snprintf(resp, sizeof(resp), "{\"rot\":%d,\"quality\":%d}",
			 rot, quality);
		send_json_str(fd, resp);
	} else if (strcmp(path, "/api/info") == 0) {
		snprintf(resp, sizeof(resp),
			 "{\"arch\":\"bk7258\",\"lan_ip\":\"%s\","
			 "\"public_ip\":null,\"geo\":null,\"weather\":null}",
			 s_lan_ip[0] ? s_lan_ip : "0.0.0.0");
		send_json_str(fd, resp);
	} else if (strcmp(path, "/stream") == 0 || strcmp(path, "/capture") == 0) {
		const char *msg = "camera streaming not wired yet";

		send_reply(fd, 503, "text/plain", NULL, msg, strlen(msg));
	} else {
		/* embedded /public/* files */
		int found = 0;

		for (unsigned i = 0; i < embedded_files_count; i++) {
			if (strcmp(path, embedded_files[i].path) == 0) {
				send_reply(fd, 200, embedded_files[i].ctype, NULL,
					   embedded_files[i].data,
					   embedded_files[i].len);
				found = 1;
				break;
			}
		}
		if (!found)
			send_reply(fd, 404, "text/plain", NULL, "not found", 9);
	}

	close(fd);
}

static void http_server_task(beken_thread_arg_t arg)
{
	int srv = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr;
	int one = 1;

	(void)arg;

	if (srv < 0) {
		printf("http: socket() failed\r\n");
		return;
	}
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(HTTP_PORT);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		printf("http: bind() failed\r\n");
		close(srv);
		return;
	}
	listen(srv, 4);
	printf("http: listening on port %d\r\n", HTTP_PORT);

	while (1) {
		struct sockaddr_in cli;
		socklen_t clen = sizeof(cli);
		int fd = accept(srv, (struct sockaddr *)&cli, &clen);

		if (fd >= 0) {
			handle_client(fd);
		} else {
			rtos_delay_milliseconds(10);
		}
	}
}

void user_app_main(void)
{
	printf("\r\n========================================\r\n");
	printf("  migan-bk7258  http_server (e_server)\r\n");
	printf("========================================\r\n");

	gpio_out_init(LED1_GPIO, 0);
	gpio_out_init(LED2_GPIO, 0);

	temp_detect_init(25);
	camera_probe();

	rtos_init_semaphore_ex(&s_conn_sem, 2, 0);

	if (wifi_connect() == 0) {
		printf("http_server ready: http://%s/\r\n", s_lan_ip);
		rtos_create_thread(NULL, 5, "http", http_server_task, 8192, 0);
	} else {
		printf("wifi: not connected - web server not started\r\n");
	}

	while (1)
		rtos_delay_milliseconds(1000);
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
