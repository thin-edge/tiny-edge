/* SPDX-License-Identifier: Apache-2.0 */

/* Shared between the Zephyr port's own sources; not part of the port
 * interface. */

#ifndef TEDGE_PORT_ZEPHYR_H_
#define TEDGE_PORT_ZEPHYR_H_

/* One TLS security tag per role, from CONFIG_TEDGE_TLS_TAG_BASE up. */
#define TAG_SERVER_CA (CONFIG_TEDGE_TLS_TAG_BASE + 0)
#define TAG_DEVICE    (CONFIG_TEDGE_TLS_TAG_BASE + 1)
/* The extra anchors for downloads outside the tenant: one tag per buffer
 * given to tedge_port_tls_set_extra_ca(), from this one up. */
#define TAG_DOWNLOAD_CA_BASE (CONFIG_TEDGE_TLS_TAG_BASE + 2)
#if defined(CONFIG_TEDGE_TRUSTED_ROOTS_MAX)
/* The compiled-in bundle and each stored root. */
#define TAG_DOWNLOAD_CA_MAX  (1 + CONFIG_TEDGE_TRUSTED_ROOTS_MAX)
#else
#define TAG_DOWNLOAD_CA_MAX  2
#endif

#endif /* TEDGE_PORT_ZEPHYR_H_ */
