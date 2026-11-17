/* SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi for NuttX applications (<tedge/nuttx.h>): join a network with
 * NuttX's wireless library (wapi), take an address over DHCP, and start
 * NTP, since TLS needs the right time. The network is kept in
 * CONFIG_TINY_EDGE_WIFI_FILE: two lines, the name and the password, on the
 * same flash file system as the client's settings. It is never logged.
 */

#include <nuttx/config.h>

#include <tedge/nuttx.h>
#include <tedge/port/log.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "netutils/netlib.h"
#include "netutils/ntpclient.h"
#include "wireless/wapi.h"

TEDGE_LOG_DECLARE();

#define IFNAME CONFIG_TINY_EDGE_INTERFACE
#define JOIN_ATTEMPTS 5

/* One join at a time: the application's at boot, set-up's later. */
static pthread_mutex_t join_lock = PTHREAD_MUTEX_INITIALIZER;

static int read_stored(char *ssid, size_t ssid_len, char *psk, size_t psk_len)
{
	FILE *f = fopen(CONFIG_TINY_EDGE_WIFI_FILE, "r");

	if (f == NULL) {
		return errno == ENOENT ? -ENOENT : -errno;
	}
	ssid[0] = psk[0] = '\0';
	if (fgets(ssid, (int)ssid_len, f) == NULL) {
		(void)fclose(f);
		return -ENOENT;
	}
	if (fgets(psk, (int)psk_len, f) == NULL) {
		psk[0] = '\0';
	}
	(void)fclose(f);
	ssid[strcspn(ssid, "\r\n")] = '\0';
	psk[strcspn(psk, "\r\n")] = '\0';
	return ssid[0] != '\0' ? 0 : -ENOENT;
}

/* Written to a temporary file and renamed, so a reset part way through
 * leaves the old network, not half of a new one. */
static int store(const char *ssid, const char *psk)
{
	static const char tmp[] = CONFIG_TINY_EDGE_WIFI_FILE ".tmp";
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	int ret = 0;

	if (fd < 0) {
		return -errno;
	}
	if (dprintf(fd, "%s\n%s\n", ssid, psk) < 0 || fsync(fd) != 0) {
		ret = -EIO;
	}
	(void)close(fd);
	if (ret == 0 && rename(tmp, CONFIG_TINY_EDGE_WIFI_FILE) != 0) {
		ret = -errno;
	}
	if (ret != 0) {
		(void)unlink(tmp);
	}
	return ret;
}

/* Power save delays what the device receives until the next beacon it
 * wakes for, often past 100 ms. NuttX's NTP client pings the gateway with
 * a 100 ms timeout before it asks for the time, and skips the round when no
 * reply came back, so with power save on the clock was never set. */
static void no_power_save(void)
{
	int sock = wapi_make_socket();

	if (sock < 0) {
		return;
	}
	if (wapi_set_power_save(sock, IFNAME, false) < 0) {
		TEDGE_LOG_WRN("Wi-Fi: could not turn power save off");
	}
	(void)close(sock);
}

/* After a join that timed out, the ESP chips' driver keeps trying in the
 * background: every later join, and every scan, is refused at once until
 * it stops. */
static void stop_joining(void)
{
	int sock = wapi_make_socket();

	if (sock >= 0) {
		wpa_driver_wext_disconnect(sock, IFNAME);
		(void)close(sock);
	}
}

static int associate(const char *ssid, const char *psk)
{
	struct wpa_wconfig_s conf = {
		.sta_mode = WAPI_MODE_MANAGED,
		.auth_wpa = IW_AUTH_WPA_VERSION_WPA2,
		.cipher_mode = IW_AUTH_CIPHER_CCMP,
		.alg = WPA_ALG_CCMP,
		.ifname = IFNAME,
		.ssid = ssid,
		.ssidlen = (uint8_t)strlen(ssid),
		.passphrase = psk,
		.phraselen = (uint8_t)strlen(psk),
	};
	int ret;

	if (conf.phraselen == 0) {
		conf.auth_wpa = IW_AUTH_WPA_VERSION_DISABLED;
		conf.cipher_mode = IW_AUTH_CIPHER_NONE;
		conf.alg = WPA_ALG_NONE;
	}
	ret = netlib_ifup(IFNAME);
	if (ret < 0) {
		return -EIO;
	}
	ret = wpa_driver_wext_associate(&conf);
	if (ret < 0) {
		TEDGE_LOG_WRN("Wi-Fi: could not join the network (%d)", ret);
		stop_joining();
		return ret;
	}
	ret = netlib_obtain_ipv4addr(IFNAME);
	if (ret < 0) {
		TEDGE_LOG_WRN("Wi-Fi: no address from DHCP (%d)", ret);
		stop_joining();
		return -ETIMEDOUT;
	}
	TEDGE_LOG_INF("Wi-Fi: connected");
	no_power_save();
	/* Already running after an earlier join: that is fine. */
	(void)ntpc_start();
	return 0;
}

/* The first join after boot, or after a failed join, often times out on
 * the ESP chips (the driver is still recovering), so a stored network is
 * tried a few times. Called with join_lock held. */
static int associate_retrying(const char *ssid, const char *psk)
{
	int ret = -EIO;

	for (int i = 0; i < JOIN_ATTEMPTS; i++) {
		ret = associate(ssid, psk);
		if (ret == 0) {
			break;
		}
		sleep(2);
	}
	return ret;
}

int tedge_nuttx_wifi_join_stored(void)
{
	char ssid[33];
	char psk[65];
	int ret;

	(void)pthread_mutex_lock(&join_lock);
	ret = read_stored(ssid, sizeof(ssid), psk, sizeof(psk));
	if (ret == 0) {
		ret = associate_retrying(ssid, psk);
	}
	memset(psk, 0, sizeof(psk));
	(void)pthread_mutex_unlock(&join_lock);
	return ret;
}

int tedge_nuttx_wifi_join(const char *ssid, const char *password)
{
	char old_ssid[33];
	char old_psk[65];
	int ret;

	if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32 ||
	    (password != NULL && strlen(password) > 64)) {
		return -EINVAL;
	}
	if (password == NULL) {
		password = "";
	}
	(void)pthread_mutex_lock(&join_lock);
	ret = associate(ssid, password);
	if (ret == 0) {
		ret = store(ssid, password);
		if (ret != 0) {
			TEDGE_LOG_ERR("Wi-Fi: joined, but could not store the "
				      "network (%d)", ret);
		}
	} else if (read_stored(old_ssid, sizeof(old_ssid), old_psk,
			       sizeof(old_psk)) == 0) {
		TEDGE_LOG_INF("Wi-Fi: going back to the stored network");
		if (associate_retrying(old_ssid, old_psk) != 0) {
			TEDGE_LOG_WRN("Wi-Fi: the stored network could not be "
				      "joined either");
		}
	}
	memset(old_psk, 0, sizeof(old_psk));
	(void)pthread_mutex_unlock(&join_lock);
	return ret;
}

/* Networks the scan found with the same name (several access points) are
 * listed once, with the strongest signal. */
static size_t add_network(struct tedge_provisioning_network *out, size_t n,
			  size_t max, const struct wapi_scan_info_s *ap)
{
	int8_t rssi = ap->has_rssi ? (int8_t)ap->rssi : -100;
	bool secured = !ap->has_encode || !(ap->encode & IW_ENCODE_DISABLED);

	for (size_t i = 0; i < n; i++) {
		if (strcmp(out[i].ssid, ap->essid) == 0) {
			if (rssi > out[i].rssi) {
				out[i].rssi = rssi;
				out[i].secured = secured;
			}
			return n;
		}
	}
	if (n == max) {
		/* Full: replace the weakest, if this one is stronger. */
		size_t weakest = 0;

		for (size_t i = 1; i < n; i++) {
			if (out[i].rssi < out[weakest].rssi) {
				weakest = i;
			}
		}
		if (rssi <= out[weakest].rssi) {
			return n;
		}
		n--;
		out[weakest] = out[n];
	}
	snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", ap->essid);
	out[n].rssi = rssi;
	out[n].secured = secured;
	return n + 1;
}

static int by_signal(const void *a, const void *b)
{
	const struct tedge_provisioning_network *x = a, *y = b;

	return (int)y->rssi - (int)x->rssi;
}

int tedge_nuttx_wifi_scan(struct tedge_provisioning_network *out, size_t max)
{
	struct wapi_list_s list = { 0 };
	size_t n = 0;
	int sock, ret;

	if (out == NULL || max == 0) {
		return -EINVAL;
	}
	sock = wapi_make_socket();
	if (sock < 0) {
		return -EIO;
	}
	/* Not while a join is under way: the driver does one at a time. */
	(void)pthread_mutex_lock(&join_lock);
	(void)netlib_ifup(IFNAME);
	ret = wapi_scan_init(sock, IFNAME, NULL);
	if (ret < 0) {
		/* Refused while the driver is still trying to join. */
		wpa_driver_wext_disconnect(sock, IFNAME);
		ret = wapi_scan_init(sock, IFNAME, NULL);
	}
	/* Up to 15 s, as the provisioning library allows. */
	for (int i = 0; ret >= 0 && i < 75; i++) {
		ret = wapi_scan_stat(sock, IFNAME);
		if (ret != 1) {
			break;
		}
		usleep(200 * 1000);
	}
	if (ret == 0) {
		ret = wapi_scan_coll(sock, IFNAME, &list);
	} else if (ret > 0) {
		ret = -ETIMEDOUT;
	}
	(void)pthread_mutex_unlock(&join_lock);
	(void)close(sock);
	if (ret < 0) {
		/* wapi returns -1 and sets errno. */
		ret = (ret == -1 && errno != 0) ? -errno : ret;
		TEDGE_LOG_WRN("Wi-Fi: scan failed (%d)", ret);
		return ret;
	}
	for (struct wapi_scan_info_s *ap = list.head.scan; ap != NULL;
	     ap = ap->next) {
		if (ap->has_essid && ap->essid[0] != '\0') {
			n = add_network(out, n, max, ap);
		}
	}
	wapi_scan_coll_free(&list);
	qsort(out, n, sizeof(*out), by_signal);
	return (int)n;
}
