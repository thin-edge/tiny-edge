/* SPDX-License-Identifier: Apache-2.0
 *
 * Onboarding with the Cumulocity bootstrap user (the fallback when the device
 * has no certificate): connect to Core MQTT as the tenant's bootstrap user,
 * ask for device credentials on s/ucr until the operator accepts the device,
 * and store the "70,<tenant>,<user>,<password>" reply.
 *
 * It uses its own short-lived MQTT session so the main one always runs as the
 * device. Passwords are never logged.
 */

#include "tedge_internal.h"

#include "tedge_mqtt.h"
#include "tedge_util.h"

#include <tedge/port/kv.h>
#include <tedge/port/log.h>
#include <tedge/port/net.h>
#include <tedge/port/os.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

TEDGE_LOG_DECLARE();

#define BOOTSTRAP_PORT   8883
#define POLL_INTERVAL_MS 5000
#define WINDOW_MS        (2 * 60 * 1000)

static char dev_user[96];
static char dev_password[96];
static bool have_credentials;

/* Runtime credentials from tedge_set_bootstrap_credentials(). */
static char boot_user[96] = TEDGE_BOOTSTRAP_USER;
static char boot_password[96] = TEDGE_BOOTSTRAP_PASSWORD;

static struct tedge_mqtt client;
/* The credentials reply, with its topic and header in front. */
static uint8_t packet_buf[256 + 32];

int tedge_set_bootstrap_credentials(const char *user, const char *password)
{
	if (!TEDGE_IS_ENABLED(TEDGE_AUTH_BOOTSTRAP)) {
		return -ENOTSUP;
	}
	if (user == NULL || password == NULL || strlen(user) >= sizeof(boot_user) ||
	    strlen(password) >= sizeof(boot_password)) {
		return -EINVAL;
	}
	strcpy(boot_user, user);
	strcpy(boot_password, password);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Settings                                                                  */
/* ------------------------------------------------------------------------ */

/* A stored string, without its NUL, into @p buf. */
static void load_string(const char *key, char *buf, size_t cap)
{
	int n = tedge_port_kv_get(key, buf, cap - 1);

	buf[(n > 0) ? n : 0] = '\0';
}

static void load_stored(void)
{
	load_string(TEDGE_KEY_BOOTSTRAP_USER, dev_user, sizeof(dev_user));
	load_string(TEDGE_KEY_BOOTSTRAP_PASS, dev_password, sizeof(dev_password));
	have_credentials = (dev_user[0] != '\0' && dev_password[0] != '\0');
	if (have_credentials) {
		TEDGE_LOG_INF("device credentials found for \"%s\"", dev_user);
	}
}

/* ------------------------------------------------------------------------ */
/* The bootstrap session                                                     */
/* ------------------------------------------------------------------------ */

/* "70,<tenant>,<user>,<password>" */
static void parse_credentials(const char *line)
{
	char tenant[48], user[48], password[64];

	if (tedge_sr_template(line) != 70) {
		return;
	}
	if (tedge_sr_field(line, 1, tenant, sizeof(tenant)) < 0 ||
	    tedge_sr_field(line, 2, user, sizeof(user)) < 0 ||
	    tedge_sr_field(line, 3, password, sizeof(password)) < 0) {
		TEDGE_LOG_ERR("malformed device credentials");
		return;
	}
	snprintf(dev_user, sizeof(dev_user), "%s/%s", tenant, user);
	snprintf(dev_password, sizeof(dev_password), "%s", password);
	(void)tedge_port_kv_set(TEDGE_KEY_BOOTSTRAP_USER, dev_user,
				strlen(dev_user));
	(void)tedge_port_kv_set(TEDGE_KEY_BOOTSTRAP_PASS, dev_password,
				strlen(dev_password));
	memset(password, 0, sizeof(password));
	have_credentials = true;
	TEDGE_LOG_INF("device credentials received for \"%s\"", dev_user);
}

static void on_message(const char *topic, const char *payload, size_t len,
		       bool dropped, void *ctx)
{
	TEDGE_UNUSED(topic);
	TEDGE_UNUSED(len);
	TEDGE_UNUSED(ctx);
	if (!dropped) {
		parse_credentials(payload);
	}
}

/* Read input for up to @p ms, or until the credentials have come. */
static int pump(int ms)
{
	int64_t end = tedge_port_uptime_ms() + ms;

	while (!have_credentials && tedge_port_uptime_ms() < end) {
		int ret = tedge_mqtt_poll(&client, 500);

		if (ret != 0) {
			return ret;
		}
	}
	return 0;
}

static int bootstrap_session(void)
{
	const struct tedge_mqtt_connect opts = {
		/* The client ID is the device's external ID, not the
		 * bootstrap user: that is how Cumulocity knows which
		 * registration is asking. */
		.client_id = tedge_identity()->external_id,
		.user = boot_user,
		.password = boot_password,
		.keepalive_s = 60,
	};
	uint8_t addr[4];
	int64_t deadline;
	int conn, ret;

	if (boot_user[0] == '\0' || boot_password[0] == '\0') {
		TEDGE_LOG_ERR("no bootstrap credentials: set "
			      TEDGE_CONFIG_NAME(TEDGE_BOOTSTRAP_USER) " and _PASSWORD, "
			      "or call tedge_set_bootstrap_credentials()");
		return -EACCES;
	}

	ret = tedge_port_resolve(tedge_c8y_host(), addr, 10000);
	if (ret != 0) {
		return -EHOSTUNREACH;
	}
	conn = tedge_port_connect(addr, BOOTSTRAP_PORT, TEDGE_PORT_TLS,
				  tedge_c8y_host(), 15000);
	if (conn < 0) {
		TEDGE_LOG_ERR("bootstrap connect failed (%d)", conn);
		return conn;
	}
	tedge_mqtt_init(&client, conn, packet_buf, sizeof(packet_buf),
			on_message, NULL);
	ret = tedge_mqtt_connect(&client, &opts, 15000);
	if (ret != 0) {
		TEDGE_LOG_ERR("no bootstrap CONNACK (ret=%d result=%d)", ret,
			      client.connack_code);
		tedge_port_close(conn);
		return -ECONNREFUSED;
	}
	(void)tedge_mqtt_subscribe(&client, "s/dcr", 3000);
	(void)pump(500);

	TEDGE_LOG_INF("waiting for the device registration to be accepted");
	deadline = tedge_port_uptime_ms() + WINDOW_MS;
	while (!have_credentials && tedge_port_uptime_ms() < deadline) {
		(void)tedge_mqtt_publish(&client, "s/ucr", NULL, 0, 0);
		ret = pump(POLL_INTERVAL_MS);
		if (ret != 0) {
			break;
		}
	}
	tedge_mqtt_disconnect(&client);
	tedge_port_sleep_ms(300);
	tedge_port_close(conn);
	return have_credentials ? 0 : -EAGAIN;
}

/* ------------------------------------------------------------------------ */
/* Interface used by the transport                                           */
/* ------------------------------------------------------------------------ */

int tedge_set_enroll_otp(const char *password)
{
	TEDGE_UNUSED(password);
	return -ENOTSUP; /* bootstrap devices have no one-time password */
}

int tedge_registration_url(char *buf, size_t len)
{
	TEDGE_UNUSED(buf);
	TEDGE_UNUSED(len);
	return -ENOTSUP; /* bootstrap devices are registered in the UI */
}

int tedge_auth_prepare(char *id_out, size_t id_len)
{
	snprintf(id_out, id_len, "%s", tedge_identity()->external_id);
	if (have_credentials) {
		return 0;
	}
	load_stored();
	if (have_credentials) {
		return 0;
	}
	tedge_set_state(TEDGE_STATE_AWAITING_REGISTRATION);
	return bootstrap_session();
}

const char *tedge_auth_username(void)
{
	return have_credentials ? dev_user : NULL;
}

const char *tedge_auth_password(void)
{
	return have_credentials ? dev_password : NULL;
}
