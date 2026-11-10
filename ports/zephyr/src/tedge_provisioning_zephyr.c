/* SPDX-License-Identifier: Apache-2.0
 *
 * Ready-made provisioning hooks for Zephyr applications
 * (<tedge/provisioning_zephyr.h>):
 *
 * - Wi-Fi: join the network on the first Wi-Fi interface, keep it in
 *   Zephyr's Wi-Fi credentials store only once the device has an address,
 *   and otherwise go back to the network it had, so a mistyped password
 *   cannot strand a device.
 * - Identify: blink the board's led0, or the first pixel of its led-strip,
 *   when the devicetree names one.
 *
 * tedge_provisioning_zephyr_init() starts the library with these, the scan
 * (tedge_provisioning_wifi.c) and the SoC's name, letting the application
 * replace any of them.
 */

#include <tedge/provisioning_zephyr.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_WIFI)
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#endif
#if defined(CONFIG_WIFI_CREDENTIALS)
#include <zephyr/net/wifi_credentials.h>
#endif
#if DT_HAS_ALIAS(led0) && defined(CONFIG_GPIO)
#include <zephyr/drivers/gpio.h>
#define HAVE_LED0 1
#endif
#if DT_HAS_ALIAS(led_strip) && defined(CONFIG_LED_STRIP)
#include <zephyr/drivers/led_strip.h>
#define HAVE_LED_STRIP 1
#endif

#include <errno.h>
#include <string.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

/* --- Wi-Fi ---------------------------------------------------------------- */

#if defined(CONFIG_WIFI) && defined(CONFIG_WIFI_CREDENTIALS)

#define JOIN_TIMEOUT    K_SECONDS(30)
#define ADDRESS_WAIT_MS 15000

static K_SEM_DEFINE(join_done, 0, 1);
static int join_status;
static struct net_mgmt_event_callback join_cb;
static bool join_cb_added;

static void join_event(struct net_mgmt_event_callback *cb, uint64_t event,
		       struct net_if *iface)
{
	ARG_UNUSED(iface);
	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *status = cb->info;

		join_status = status->status;
		k_sem_give(&join_done);
	}
}

/* Join @p ssid; 0 once the device has an IPv4 address. */
static int join(struct net_if *iface, const char *ssid, size_t ssid_len,
		const char *password, size_t password_len,
		enum wifi_security_type security)
{
	struct wifi_connect_req_params p = {
		.ssid = (const uint8_t *)ssid,
		.ssid_length = ssid_len,
		.psk = (const uint8_t *)password,
		.psk_length = password_len,
		.security = security,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_UNKNOWN,
		.mfp = WIFI_MFP_OPTIONAL,
	};
	int64_t until;
	int rc;

	(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
	k_sem_reset(&join_done);
	rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof(p));
	if (rc != 0) {
		return rc;
	}
	if (k_sem_take(&join_done, JOIN_TIMEOUT) != 0) {
		return -ETIMEDOUT;
	}
	if (join_status != 0) {
		return -ECONNREFUSED;
	}
	until = k_uptime_get() + ADDRESS_WAIT_MS;
	while (net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) == NULL) {
		if (k_uptime_get() >= until) {
			return -ETIMEDOUT;
		}
		k_msleep(250);
	}
	return 0;
}

/* The first stored network, if there is one. */
struct stored {
	struct wifi_credentials_personal creds;
	bool present;
};

static void first_ssid(void *arg, const char *ssid, size_t ssid_len)
{
	struct stored *s = arg;

	if (!s->present &&
	    wifi_credentials_get_by_ssid_personal_struct(ssid, ssid_len,
							 &s->creds) == 0) {
		s->present = true;
	}
}

static int wifi_connect(const char *ssid, const char *password, void *ctx)
{
	struct net_if *iface = net_if_get_first_wifi();
	enum wifi_security_type security = password[0] != '\0'
						   ? WIFI_SECURITY_TYPE_PSK
						   : WIFI_SECURITY_TYPE_NONE;
	static struct stored previous; /* too big for the worker's stack */
	int rc;

	ARG_UNUSED(ctx);
	if (iface == NULL) {
		return -ENODEV;
	}
	if (!join_cb_added) {
		net_mgmt_init_event_callback(&join_cb, join_event,
					     NET_EVENT_WIFI_CONNECT_RESULT);
		net_mgmt_add_event_callback(&join_cb);
		join_cb_added = true;
	}
	memset(&previous, 0, sizeof(previous));
	wifi_credentials_for_each_ssid(first_ssid, &previous);

	rc = join(iface, ssid, strlen(ssid), password, strlen(password),
		  security);
	if (rc == 0) {
		/* It works: keep it, and only it. */
		(void)wifi_credentials_delete_all();
		rc = wifi_credentials_set_personal(ssid, strlen(ssid), security,
						   NULL, 0, password,
						   strlen(password), 0, 0, 0);
		if (rc != 0) {
			LOG_ERR("provisioning: joined \"%s\" but could not store "
				"it (%d)", ssid, rc);
		}
	} else if (previous.present) {
		LOG_WRN("provisioning: could not join \"%s\" (%d); back to "
			"\"%.*s\"", ssid, rc, (int)previous.creds.header.ssid_len,
			previous.creds.header.ssid);
		(void)join(iface, previous.creds.header.ssid,
			   previous.creds.header.ssid_len,
			   previous.creds.password, previous.creds.password_len,
			   previous.creds.header.type);
	}
	memset(&previous, 0, sizeof(previous));
	return rc;
}

#endif /* CONFIG_WIFI && CONFIG_WIFI_CREDENTIALS */

/* --- Identify ------------------------------------------------------------- */

#if defined(HAVE_LED0) || defined(HAVE_LED_STRIP)

#define BLINK_MS    100
#define IDENTIFY_MS 10000

#if defined(HAVE_LED0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#endif
#if defined(HAVE_LED_STRIP)
static const struct device *const strip = DEVICE_DT_GET(DT_ALIAS(led_strip));
#endif

static int blinks_left;
static void blink(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(blink_work, blink);

static void led_set(bool on)
{
#if defined(HAVE_LED0)
	if (gpio_is_ready_dt(&led)) {
		(void)gpio_pin_set_dt(&led, on);
	}
#endif
#if defined(HAVE_LED_STRIP)
	if (device_is_ready(strip)) {
		struct led_rgb px = { .r = on ? 32 : 0, .g = on ? 32 : 0,
				      .b = on ? 32 : 0 };

		(void)led_strip_update_rgb(strip, &px, 1);
	}
#endif
}

static void blink(struct k_work *work)
{
	ARG_UNUSED(work);
	led_set(blinks_left % 2 == 0);
	if (--blinks_left > 0) {
		(void)k_work_schedule(&blink_work, K_MSEC(BLINK_MS));
	} else {
		led_set(false);
	}
}

static void identify(void *ctx)
{
	ARG_UNUSED(ctx);
#if defined(HAVE_LED0)
	if (gpio_is_ready_dt(&led)) {
		(void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
#endif
	blinks_left = IDENTIFY_MS / BLINK_MS;
	(void)k_work_reschedule(&blink_work, K_NO_WAIT);
}

#endif /* HAVE_LED0 || HAVE_LED_STRIP */

/* --- Setup ---------------------------------------------------------------- */

int tedge_provisioning_zephyr_init(const struct tedge_provisioning_hooks *overrides)
{
	static struct tedge_provisioning_hooks hooks;

	memset(&hooks, 0, sizeof(hooks));
#if defined(CONFIG_WIFI) && defined(CONFIG_WIFI_CREDENTIALS)
	hooks.wifi_connect = wifi_connect;
#endif
#if defined(CONFIG_WIFI)
	hooks.wifi_scan = tedge_provisioning_wifi_scan;
#endif
#if defined(HAVE_LED0) || defined(HAVE_LED_STRIP)
	hooks.identify = identify;
#endif
	hooks.chip = CONFIG_SOC;

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
	if (hooks.wifi_connect == NULL) {
		LOG_WRN("provisioning: no Wi-Fi hook (CONFIG_WIFI_CREDENTIALS is "
			"off): Wi-Fi settings will be refused");
	}
	return tedge_provisioning_init(&hooks);
}
