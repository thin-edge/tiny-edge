/* SPDX-License-Identifier: Apache-2.0
 *
 * The Wi-Fi scan hook for the provisioning library: one scan on the first
 * Wi-Fi interface, collected from the network management events.
 */

#include <tedge/provisioning_zephyr.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

#include <errno.h>
#include <string.h>

#define SCAN_TIMEOUT K_SECONDS(15)

static K_MUTEX_DEFINE(scan_lock);
static K_SEM_DEFINE(scan_done, 0, 1);
static struct net_mgmt_event_callback scan_cb;
static bool scan_cb_added;

/* The scan in progress; written from the event callback. */
static struct tedge_provisioning_network *scan_out;
static size_t scan_max;
static size_t scan_count;

static void scan_event(struct net_mgmt_event_callback *cb, uint64_t event,
		       struct net_if *iface)
{
	ARG_UNUSED(iface);
	if (event == NET_EVENT_WIFI_SCAN_RESULT) {
		const struct wifi_scan_result *r = cb->info;
		struct tedge_provisioning_network *n;
		size_t len;

		if (scan_out == NULL || scan_count >= scan_max) {
			return;
		}
		n = &scan_out[scan_count++];
		len = MIN(r->ssid_length, sizeof(n->ssid) - 1);
		memcpy(n->ssid, r->ssid, len);
		n->ssid[len] = '\0';
		n->rssi = r->rssi;
		n->secured = r->security != WIFI_SECURITY_TYPE_NONE;
	} else if (event == NET_EVENT_WIFI_SCAN_DONE) {
		k_sem_give(&scan_done);
	}
}

int tedge_provisioning_wifi_scan(struct tedge_provisioning_network *out,
				 size_t max, void *ctx)
{
	struct net_if *iface = net_if_get_first_wifi();
	int rc;

	ARG_UNUSED(ctx);
	if (iface == NULL) {
		return -ENODEV;
	}
	k_mutex_lock(&scan_lock, K_FOREVER);
	if (!scan_cb_added) {
		net_mgmt_init_event_callback(&scan_cb, scan_event,
					     NET_EVENT_WIFI_SCAN_RESULT |
						     NET_EVENT_WIFI_SCAN_DONE);
		net_mgmt_add_event_callback(&scan_cb);
		scan_cb_added = true;
	}
	scan_out = out;
	scan_max = max;
	scan_count = 0;
	k_sem_reset(&scan_done);
	rc = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, NULL, 0);
	if (rc == 0 && k_sem_take(&scan_done, SCAN_TIMEOUT) != 0) {
		rc = -ETIMEDOUT;
	}
	scan_out = NULL;
	k_mutex_unlock(&scan_lock);
	return (rc == 0) ? (int)scan_count : rc;
}
