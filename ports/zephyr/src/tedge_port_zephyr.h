/* SPDX-License-Identifier: Apache-2.0 */

/* Shared between the Zephyr port's own sources; not part of the port
 * interface. */

#ifndef TEDGE_PORT_ZEPHYR_H_
#define TEDGE_PORT_ZEPHYR_H_

/* One TLS security tag per role, from CONFIG_TEDGE_TLS_TAG_BASE up. */
#define TAG_SERVER_CA (CONFIG_TEDGE_TLS_TAG_BASE + 0)
#define TAG_DEVICE    (CONFIG_TEDGE_TLS_TAG_BASE + 1)

#endif /* TEDGE_PORT_ZEPHYR_H_ */
