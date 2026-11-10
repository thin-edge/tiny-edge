/* SPDX-License-Identifier: Apache-2.0
 *
 * The smallest application that hosts the thin-edge.io client: join a Wi-Fi
 * network, then start the client and let it do the rest. Everything here is
 * the *application's* job; the client only reacts to the network being up.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>

#include <tedge/tedge.h>

#include <string.h>

#if defined(CONFIG_TEDGE_PROVISIONING)
#include <tedge/provisioning.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_credentials.h>
#endif

LOG_MODULE_REGISTER(tedge_minimal, LOG_LEVEL_INF);

static void on_state(enum tedge_state state, void *user_data)
{
	ARG_UNUSED(user_data);

	if (state == TEDGE_STATE_AWAITING_REGISTRATION) {
		char url[220];

		/* The URL carries a one-time password: show it to the operator
		 * (here the console; a real device might use a display or its
		 * provisioner). */
		if (tedge_registration_url(url, sizeof(url)) > 0) {
			LOG_INF("register this device: %s", url);
		}
	}
}

static const struct tedge_hooks hooks = {
	.on_state = on_state,
};

#if defined(CONFIG_WIFI)
static int wifi_connect(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_connect_req_params p = {
		.ssid = (const uint8_t *)CONFIG_TEDGE_SAMPLE_WIFI_SSID,
		.ssid_length = strlen(CONFIG_TEDGE_SAMPLE_WIFI_SSID),
		.psk = (const uint8_t *)CONFIG_TEDGE_SAMPLE_WIFI_PSK,
		.psk_length = strlen(CONFIG_TEDGE_SAMPLE_WIFI_PSK),
		.security = strlen(CONFIG_TEDGE_SAMPLE_WIFI_PSK)
				    ? WIFI_SECURITY_TYPE_PSK
				    : WIFI_SECURITY_TYPE_NONE,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
	};

	if (iface == NULL) {
		LOG_ERR("no Wi-Fi interface");
		return -ENODEV;
	}
	LOG_INF("joining \"%s\"", CONFIG_TEDGE_SAMPLE_WIFI_SSID);
	return net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof(p));
}
#endif

#if defined(CONFIG_TEDGE_PROVISIONING)
/* Setting the device up with a tool (tedge-onboard) over the console: the
 * library hands this application the Wi-Fi network to join, and the client
 * the Cumulocity tenant. The network is stored with Zephyr's Wi-Fi
 * credentials, so the next boot joins it by itself. */

static K_SEM_DEFINE(wifi_done, 0, 1);
static int wifi_status;
static struct net_mgmt_event_callback wifi_cb;

static void wifi_event(struct net_mgmt_event_callback *cb, uint64_t event,
		       struct net_if *iface)
{
	ARG_UNUSED(iface);
	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *status = cb->info;

		wifi_status = status->status;
		k_sem_give(&wifi_done);
	}
}

static int provision_wifi(const char *ssid, const char *password, void *ctx)
{
	struct net_if *iface = net_if_get_first_wifi();
	enum wifi_security_type security = password[0] != '\0'
						   ? WIFI_SECURITY_TYPE_PSK
						   : WIFI_SECURITY_TYPE_NONE;
	struct wifi_connect_req_params p = {
		.ssid = (const uint8_t *)ssid,
		.ssid_length = strlen(ssid),
		.psk = (const uint8_t *)password,
		.psk_length = strlen(password),
		.security = security,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
	};
	int rc;

	ARG_UNUSED(ctx);
	if (iface == NULL) {
		return -ENODEV;
	}
	(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
	k_sem_reset(&wifi_done);
	rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof(p));
	if (rc != 0) {
		return rc;
	}
	if (k_sem_take(&wifi_done, K_SECONDS(30)) != 0) {
		return -ETIMEDOUT;
	}
	if (wifi_status != 0) {
		return -ECONNREFUSED;
	}
	/* Joined: keep it, and only it, for the next boot. */
	(void)wifi_credentials_delete_all();
	return wifi_credentials_set_personal(ssid, strlen(ssid), security, NULL, 0,
					     password, strlen(password), 0, 0, 0);
}

static const struct tedge_provisioning_hooks provisioning_hooks = {
	.wifi_connect = provision_wifi,
	.chip = CONFIG_SOC,
};
#endif

int main(void)
{
	LOG_INF("tedge-zephyr %s", tedge_version());

#if defined(CONFIG_TEDGE_PROVISIONING)
	net_mgmt_init_event_callback(&wifi_cb, wifi_event,
				     NET_EVENT_WIFI_CONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);
	if (!wifi_credentials_is_empty()) {
		/* Set up before: join the network it was given. */
		(void)net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED,
			       net_if_get_first_wifi(), NULL, 0);
	} else if (strlen(CONFIG_TEDGE_SAMPLE_WIFI_SSID) > 0) {
		(void)wifi_connect();
	} else {
		LOG_INF("no Wi-Fi network yet: set the device up with "
			"tedge-onboard");
	}
#elif defined(CONFIG_WIFI)
	/* The client waits for an address by itself, so a failure here only
	 * delays it: the application can retry however it likes. */
	(void)wifi_connect();
#endif

	if (tedge_init(NULL, &hooks) == 0) {
#if defined(CONFIG_TEDGE_PROVISIONING)
		(void)tedge_provisioning_init(&provisioning_hooks);
#endif
		(void)tedge_start();
	}
	return 0;
}
