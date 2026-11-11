/* SPDX-License-Identifier: Apache-2.0
 *
 * The smallest ESP-IDF application that hosts the thin-edge.io client:
 * join Wi-Fi, start the client, and publish a measurement now and then.
 * The client connects, enrolls and reports by itself.
 *
 * With certificate enrollment (the default), the first boot logs a URL:
 * open it to register the device in Cumulocity. The key and certificate
 * are kept in NVS, so later boots connect at once.
 */

#include <tedge/tedge.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

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
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	(void)arg;
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
		(void)esp_wifi_connect();
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		const wifi_event_sta_disconnected_t *ev = data;

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
	strncpy((char *)cfg.sta.ssid, CONFIG_SAMPLE_WIFI_SSID,
		sizeof(cfg.sta.ssid));
	strncpy((char *)cfg.sta.password, CONFIG_SAMPLE_WIFI_PASSWORD,
		sizeof(cfg.sta.password));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
	ESP_ERROR_CHECK(esp_wifi_start());
}

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

	ESP_LOGI(TAG, "tiny-edge %s", tedge_version());
	if (tedge_init(NULL, &hooks) != 0 || tedge_start() != 0) {
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
