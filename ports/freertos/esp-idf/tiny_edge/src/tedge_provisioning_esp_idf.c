/* SPDX-License-Identifier: Apache-2.0
 *
 * Ready-made provisioning hooks for ESP-IDF applications
 * (<tedge/provisioning_esp_idf.h>).
 *
 * Improv Serial on the console. ESP-IDF's console is a UART, the USB
 * Serial/JTAG port, or both (a UART console with the USB port as its
 * secondary output, the default on chips that have one). The library reads
 * every one of them, so a tool finds the board on whichever USB connector
 * is plugged in, and writes each frame to all of them, as the log does.
 * Reading needs ESP-IDF's driver for the port: it is installed here, and the
 * console's own output switched to it, unless the application did both.
 *
 * A frame is written with stdout locked: every log line goes through stdout
 * (ESP_LOG prints with vprintf), so none lands inside a frame. The frame
 * itself goes to the driver, not to stdout, because stdout turns each 0x0A
 * byte into CR LF.
 *
 * Wi-Fi: join with esp_wifi, keeping the network in RAM until the device
 * has an address and only then in NVS; when the network cannot be joined,
 * go back to the previous one, so a mistyped password cannot strand a
 * device.
 */

#include <tedge/port/config.h>
#include <tedge/port/log.h>
#include <tedge/port/net.h>
#include <tedge/port/os.h>
#include <tedge/provisioning_esp_idf.h>

#include <esp_event.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <sdkconfig.h>

#if defined(CONFIG_ESP_CONSOLE_UART)
#include <driver/uart.h>
#include <driver/uart_vfs.h>
#define HAVE_UART_CONSOLE 1
#endif
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) || \
	defined(CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#define HAVE_USB_CONSOLE 1
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEDGE_LOG_DECLARE();

/* --- Improv Serial on the console ---------------------------------------- */

#if defined(TEDGE_PROVISIONING_SERIAL)

#define READ_TICKS  pdMS_TO_TICKS(10)
#define WRITE_TICKS pdMS_TO_TICKS(50)

TEDGE_PORT_STACK_DEFINE(rx_stack, 3072);
static tedge_port_thread_t rx_thread;

static void console_write(const void *data, size_t len, void *ctx)
{
	(void)ctx;
	/* A log line is flushed by the call that prints it (stdout is line
	 * buffered), so nothing is left half-written; the lock keeps the next
	 * one out until the frame is out. */
	fflush(stdout);
	flockfile(stdout);
#if defined(HAVE_UART_CONSOLE)
	(void)uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM, data, len);
#endif
#if defined(HAVE_USB_CONSOLE)
	(void)usb_serial_jtag_write_bytes(data, len, WRITE_TICKS);
#endif
	funlockfile(stdout);
}

static void rx_loop(void *arg)
{
	uint8_t buf[64];
	int n;

	(void)arg;
	for (;;) {
#if defined(HAVE_UART_CONSOLE)
		n = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buf, sizeof(buf),
				    READ_TICKS);
		if (n > 0) {
			(void)tedge_provisioning_serial_feed(buf, (size_t)n);
		}
#endif
#if defined(HAVE_USB_CONSOLE)
		n = usb_serial_jtag_read_bytes(buf, sizeof(buf), READ_TICKS);
		if (n > 0) {
			(void)tedge_provisioning_serial_feed(buf, (size_t)n);
		}
#endif
	}
}

static int serial_start(void)
{
#if defined(HAVE_UART_CONSOLE)
	if (!uart_is_driver_installed(CONFIG_ESP_CONSOLE_UART_NUM)) {
		esp_err_t err = uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM,
						    256, 0, 0, NULL, 0);

		if (err != ESP_OK) {
			TEDGE_LOG_ERR("provisioning: no UART driver (%s)",
				      esp_err_to_name(err));
			return -EIO;
		}
		uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
	}
#endif
#if defined(HAVE_USB_CONSOLE)
	if (!usb_serial_jtag_is_driver_installed()) {
		usb_serial_jtag_driver_config_t cfg =
			USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
		esp_err_t err = usb_serial_jtag_driver_install(&cfg);

		if (err != ESP_OK) {
			TEDGE_LOG_ERR("provisioning: no USB Serial/JTAG driver (%s)",
				      esp_err_to_name(err));
			return -EIO;
		}
		usb_serial_jtag_vfs_use_driver();
	}
#endif
#if !defined(HAVE_UART_CONSOLE) && !defined(HAVE_USB_CONSOLE)
	TEDGE_LOG_WRN("provisioning: the console is neither a UART nor USB "
		      "Serial/JTAG; Improv Serial is off");
	return -ENOTSUP;
#else
	tedge_provisioning_serial_attach(console_write, NULL);
	return tedge_port_thread_start(&rx_thread, rx_stack,
				       TEDGE_PORT_STACK_SIZEOF(rx_stack), rx_loop,
				       NULL, "tedge_improv");
#endif
}

#endif /* TEDGE_PROVISIONING_SERIAL */

/* --- Wi-Fi ---------------------------------------------------------------- */

#define JOIN_MS   30000
#define GOT_IP    BIT0

static EventGroupHandle_t join_events;
static StaticEventGroup_t join_events_buf;

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	(void)arg;
	(void)base;
	(void)id;
	(void)data;
	xEventGroupSetBits(join_events, GOT_IP);
}

static int join(const wifi_config_t *cfg)
{
	xEventGroupClearBits(join_events, GOT_IP);
	(void)esp_wifi_disconnect();
	if (esp_wifi_set_config(WIFI_IF_STA, (wifi_config_t *)cfg) != ESP_OK) {
		return -EINVAL;
	}
	if (esp_wifi_connect() != ESP_OK) {
		return -EIO;
	}
	return (xEventGroupWaitBits(join_events, GOT_IP, pdTRUE, pdTRUE,
				    pdMS_TO_TICKS(JOIN_MS)) & GOT_IP)
		       ? 0
		       : -ETIMEDOUT;
}

static int wifi_connect(const char *ssid, const char *password, void *ctx)
{
	/* Static: two configurations are more than the worker's stack should
	 * carry, and only the worker calls this. */
	static wifi_config_t cfg, previous;
	bool had_previous;
	int rc;

	(void)ctx;
	if (strlen(ssid) >= sizeof(cfg.sta.ssid) ||
	    strlen(password) >= sizeof(cfg.sta.password)) {
		return -EINVAL;
	}
	memset(&cfg, 0, sizeof(cfg));
	memset(&previous, 0, sizeof(previous));
	memcpy(cfg.sta.ssid, ssid, strlen(ssid));
	memcpy(cfg.sta.password, password, strlen(password));
	had_previous = esp_wifi_get_config(WIFI_IF_STA, &previous) == ESP_OK &&
		       previous.sta.ssid[0] != '\0';

	/* Stored in flash as it is set: ESP-IDF writes NVS only when the new
	 * configuration differs from the one in RAM, so joining in RAM first and
	 * storing afterwards never reaches flash. A failed join puts the
	 * previous network back, or clears the stored one. */
	(void)esp_wifi_set_storage(WIFI_STORAGE_FLASH);
	rc = join(&cfg);
	if (rc != 0 && had_previous) {
		TEDGE_LOG_WRN("provisioning: could not join \"%s\" (%d); back to "
			      "\"%s\"", ssid, rc, (const char *)previous.sta.ssid);
		(void)join(&previous);
	} else if (rc != 0) {
		TEDGE_LOG_WRN("provisioning: could not join \"%s\" (%d)", ssid,
			      rc);
		(void)esp_wifi_disconnect();
		memset(&previous, 0, sizeof(previous));
		(void)esp_wifi_set_config(WIFI_IF_STA, &previous);
	}
	memset(&cfg, 0, sizeof(cfg));
	memset(&previous, 0, sizeof(previous));
	return rc;
}

static int wifi_scan(struct tedge_provisioning_network *out, size_t max,
		     void *ctx)
{
	wifi_ap_record_t *records;
	uint16_t n = (uint16_t)max;
	esp_err_t err = ESP_FAIL;
	size_t count = 0;

	(void)ctx;
	/* A station still trying to join cannot scan: stop trying. */
	if (!tedge_port_net_is_up()) {
		(void)esp_wifi_disconnect();
	}
	for (int i = 0; i < 3; i++) {
		err = esp_wifi_scan_start(NULL, true);
		if (err != ESP_ERR_WIFI_STATE) {
			break;
		}
		tedge_port_sleep_ms(500);
	}
	if (err != ESP_OK) {
		TEDGE_LOG_WRN("provisioning: Wi-Fi scan failed (%s)",
			      esp_err_to_name(err));
		return -EIO;
	}
	records = calloc(max, sizeof(*records));
	if (records == NULL) {
		(void)esp_wifi_clear_ap_list();
		return -ENOMEM;
	}
	if (esp_wifi_scan_get_ap_records(&n, records) == ESP_OK) {
		for (uint16_t i = 0; i < n && count < max; i++) {
			struct tedge_provisioning_network *net = &out[count];

			if (records[i].ssid[0] == '\0') {
				continue; /* a hidden network */
			}
			snprintf(net->ssid, sizeof(net->ssid), "%s",
				 (const char *)records[i].ssid);
			net->rssi = records[i].rssi;
			net->secured = records[i].authmode != WIFI_AUTH_OPEN;
			count++;
		}
	}
	free(records);
	return (int)count;
}

/* --- Setup ---------------------------------------------------------------- */

int tedge_provisioning_esp_idf_init(const struct tedge_provisioning_hooks *overrides)
{
	static struct tedge_provisioning_hooks hooks;
	int rc;

	if (join_events == NULL) {
		join_events = xEventGroupCreateStatic(&join_events_buf);
		if (esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip,
					       NULL) != ESP_OK) {
			TEDGE_LOG_ERR("provisioning: no IP events (is the default "
				      "event loop created?)");
			return -EIO;
		}
	}
	memset(&hooks, 0, sizeof(hooks));
	hooks.wifi_connect = wifi_connect;
	hooks.wifi_scan = wifi_scan;
	hooks.chip = CONFIG_IDF_TARGET;
	if (overrides != NULL) {
		if (overrides->wifi_connect != NULL) {
			hooks.wifi_connect = overrides->wifi_connect;
		}
		if (overrides->identify != NULL) {
			hooks.identify = overrides->identify;
		}
		if (overrides->wifi_scan != NULL) {
			hooks.wifi_scan = overrides->wifi_scan;
		}
		if (overrides->chip != NULL) {
			hooks.chip = overrides->chip;
		}
		if (overrides->firmware_name != NULL) {
			hooks.firmware_name = overrides->firmware_name;
		}
		if (overrides->firmware_version != NULL) {
			hooks.firmware_version = overrides->firmware_version;
		}
		hooks.ctx = overrides->ctx;
	}
	rc = tedge_provisioning_init(&hooks);
	if (rc != 0) {
		return rc;
	}
#if defined(TEDGE_PROVISIONING_SERIAL)
	rc = serial_start();
#endif
	return rc;
}
