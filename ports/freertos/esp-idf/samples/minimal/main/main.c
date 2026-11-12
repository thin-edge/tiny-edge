/* SPDX-License-Identifier: Apache-2.0
 *
 * The smallest ESP-IDF application that hosts the thin-edge.io client:
 * join Wi-Fi, start the client, and publish a measurement now and then.
 * The client connects, enrolls and reports by itself.
 *
 * With certificate enrollment (the default), the first boot logs a URL:
 * open it to register the device in Cumulocity. The key and certificate
 * are kept in NVS, so later boots connect at once.
 *
 * With the provisioning library (CONFIG_TEDGE_PROVISIONING), tedge-onboard
 * sets the device up over USB instead: the Wi-Fi network, the tenant, and
 * the registration, with nothing built into the image.
 */

#include <tedge/tedge.h>
#if CONFIG_TEDGE_PROVISIONING
#include <tedge/provisioning_esp_idf.h>
#endif

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#if CONFIG_TEDGE_SHELL_COMMAND
#include <esp_console.h>
#include <esp_heap_caps.h>
#include <esp_app_desc.h>
#endif

#include <stdbool.h>
#include <string.h>

static const char *TAG = "sample";

static void on_state(enum tedge_state state, void *user_data)
{
	(void)user_data;
	if (state == TEDGE_STATE_AWAITING_REGISTRATION) {
		char url[220];

		/* The URL carries a one-time password: it is for the person
		 * at the console. */
		if (tedge_registration_url(url, sizeof(url)) > 0) {
			ESP_LOGI(TAG, "register this device: %s", url);
		}
	}
}

static const struct tedge_hooks hooks = {
	.on_state = on_state,
};

/* Keeps the station connected: the client only watches the network. */
/* Whether the station has a network to join. */
static bool have_network(void)
{
	wifi_config_t cfg;

	return esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK &&
	       cfg.sta.ssid[0] != '\0';
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	(void)arg;
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
		if (have_network()) {
			(void)esp_wifi_connect();
		}
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		const wifi_event_sta_disconnected_t *ev = data;

		/* Leaving on purpose (tedge-onboard switching networks, or
		 * scanning) is not a reason to join again. */
		if (ev->reason == WIFI_REASON_ASSOC_LEAVE || !have_network()) {
			return;
		}
		ESP_LOGW(TAG, "Wi-Fi: not connected (reason %d), trying again",
			 ev->reason);
		vTaskDelay(pdMS_TO_TICKS(1000));
		(void)esp_wifi_connect();
	} else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		const ip_event_got_ip_t *ev = data;

		ESP_LOGI(TAG, "Wi-Fi: address " IPSTR, IP2STR(&ev->ip_info.ip));
	}
}

static void wifi_start(void)
{
	wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
	wifi_config_t cfg = { 0 };

	esp_netif_create_default_wifi_sta();
	ESP_ERROR_CHECK(esp_wifi_init(&init));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
						   on_wifi, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
						   on_wifi, NULL));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	if (CONFIG_SAMPLE_WIFI_SSID[0] != '\0') {
		strncpy((char *)cfg.sta.ssid, CONFIG_SAMPLE_WIFI_SSID,
			sizeof(cfg.sta.ssid));
		strncpy((char *)cfg.sta.password, CONFIG_SAMPLE_WIFI_PASSWORD,
			sizeof(cfg.sta.password));
		/* ESP-IDF keeps it in NVS, for images built without one. */
		ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
	} else if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK ||
		   cfg.sta.ssid[0] == '\0') {
#if CONFIG_TEDGE_PROVISIONING
		ESP_LOGW(TAG, "no Wi-Fi network yet: set the device up with "
			 "tedge-onboard");
#else
		ESP_LOGE(TAG, "no Wi-Fi network: set one in menuconfig "
			 "(\"tiny-edge sample\")");
#endif
	} else {
		ESP_LOGI(TAG, "Wi-Fi: using the network stored on the device");
	}
	ESP_ERROR_CHECK(esp_wifi_start());
}

#if CONFIG_TEDGE_SHELL_COMMAND
/* Commands the cloud can run (c8y_Command), once they are on
 * CONFIG_TEDGE_SHELL_COMMAND_ALLOW_LIST. What they print is the result. */
static int cmd_free(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("heap: %u bytes free, %u at the lowest\n",
	       (unsigned int)esp_get_free_heap_size(),
	       (unsigned int)esp_get_minimum_free_heap_size());
	printf("largest free block: %u bytes\n",
	       (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
	return 0;
}

static int cmd_version(int argc, char **argv)
{
	const esp_app_desc_t *app = esp_app_get_description();

	(void)argc;
	(void)argv;
	printf("%s %s, tiny-edge %s, ESP-IDF %s\n", app->project_name,
	       app->version, tedge_version(), app->idf_ver);
	return 0;
}

static int cmd_uptime(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("%lu s\n", (unsigned long)(xTaskGetTickCount() /
					  configTICK_RATE_HZ));
	return 0;
}

static void commands_register(void)
{
	static const esp_console_cmd_t cmds[] = {
		{ .command = "free", .help = "Free heap", .func = cmd_free },
		{ .command = "version", .help = "Firmware and library versions",
		  .func = cmd_version },
		{ .command = "uptime", .help = "Seconds since the start",
		  .func = cmd_uptime },
	};
	esp_console_config_t cfg = ESP_CONSOLE_CONFIG_DEFAULT();

	ESP_ERROR_CHECK(esp_console_init(&cfg));
	for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
		ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
	}
}
#endif

void app_main(void)
{
	esp_err_t e = nvs_flash_init();

	if (e == ESP_ERR_NVS_NO_FREE_PAGES ||
	    e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		/* A partition this firmware cannot read: start it afresh. */
		ESP_ERROR_CHECK(nvs_flash_erase());
		e = nvs_flash_init();
	}
	ESP_ERROR_CHECK(e);
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	wifi_start();

#if CONFIG_TEDGE_SHELL_COMMAND
	commands_register();
#endif
	ESP_LOGI(TAG, "tiny-edge %s", tedge_version());
	if (tedge_init(NULL, &hooks) != 0) {
		ESP_LOGE(TAG, "the client did not start");
		return;
	}
#if CONFIG_TEDGE_PROVISIONING
	if (tedge_provisioning_esp_idf_init(NULL) != 0) {
		ESP_LOGE(TAG, "tedge-onboard support did not start");
	}
#endif
	if (tedge_start() != 0) {
		ESP_LOGE(TAG, "the client did not start");
		return;
	}

	for (;;) {
		struct tedge_measurement_value v = {
			.series = "uptime",
			.value = (double)xTaskGetTickCount() /
				 configTICK_RATE_HZ,
			.unit = "s",
		};

		vTaskDelay(pdMS_TO_TICKS(CONFIG_SAMPLE_MEASUREMENT_INTERVAL_S *
					 1000));
		(void)tedge_publish_measurement("sample", &v, 1, 0);
	}
}
