/* SPDX-License-Identifier: Apache-2.0
 *
 * The ESP-IDF side of <tedge/port/net.h>: the interface queries, from
 * esp_netif and its IP events. The connections and TLS run over lwIP's
 * BSD sockets with code shared with other ports
 * (ports/common/src/tedge_sockets.c).
 *
 * The interface the client uses is the default one
 * (esp_netif_get_default_netif()): the Wi-Fi station, or Ethernet, as the
 * application set them up.
 */

#include <tedge/port/net.h>

#include "tedge_common.h"

#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi_types.h>

#include <string.h>

static bool addr_of(esp_netif_t *netif, esp_netif_ip_info_t *info)
{
	return netif != NULL && esp_netif_is_netif_up(netif) &&
	       esp_netif_get_ip_info(netif, info) == ESP_OK &&
	       info->ip.addr != 0;
}

bool tedge_port_net_is_up(void)
{
	esp_netif_ip_info_t info;

	return addr_of(esp_netif_get_default_netif(), &info);
}

/* --- Watching ------------------------------------------------------------ */

static void (*net_cb)(bool up);

/* Runs on the default event loop's task. An address that is lost is
 * reported late by lwIP (IP_EVENT_STA_LOST_IP waits for its timer), so the
 * station's disconnection counts as down at once. */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	(void)arg;
	(void)data;
	if (base == IP_EVENT) {
		switch (id) {
		case IP_EVENT_STA_GOT_IP:
		case IP_EVENT_ETH_GOT_IP:
		case IP_EVENT_PPP_GOT_IP:
			net_cb(true);
			return;
		case IP_EVENT_STA_LOST_IP:
		case IP_EVENT_ETH_LOST_IP:
		case IP_EVENT_PPP_LOST_IP:
			net_cb(false);
			return;
		default:
			return;
		}
	}
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		net_cb(false);
	}
}

/* The application creates the default event loop before it starts the
 * network (esp_event_loop_create_default()), so it exists by the time the
 * client starts. */
void tedge_port_net_watch(void (*cb)(bool up))
{
	net_cb = cb;
	(void)esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event,
					 NULL);
	(void)esp_event_handler_register(WIFI_EVENT,
					 WIFI_EVENT_STA_DISCONNECTED, on_event,
					 NULL);
}

/* --- Addresses ----------------------------------------------------------- */

bool tedge_port_net_is_own(const uint8_t addr[4])
{
	esp_netif_t *netif = NULL;
	uint32_t a;

	if (addr[0] == 127) {
		return true; /* the loopback */
	}
	memcpy(&a, addr, 4);
	/* Every interface, not only the default one. */
	while ((netif = esp_netif_next_unsafe(netif)) != NULL) {
		esp_netif_ip_info_t info;

		if (addr_of(netif, &info) && info.ip.addr == a) {
			return true;
		}
	}
	return false;
}

bool tedge_port_net_on_subnet(const uint8_t addr[4])
{
	esp_netif_ip_info_t info;
	uint32_t a;

	if (!addr_of(esp_netif_get_default_netif(), &info)) {
		return false;
	}
	memcpy(&a, addr, 4);
	return (a & info.netmask.addr) == (info.ip.addr & info.netmask.addr);
}

/* --- Sockets ------------------------------------------------------------- */

/* lwIP sockets need nothing more: no exec, and no SIGPIPE. */
void tedge_common_socket_opened(int fd)
{
	(void)fd;
}
