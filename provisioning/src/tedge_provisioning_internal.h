/* SPDX-License-Identifier: Apache-2.0 */

/* Shared between the provisioning library's sources; not public. */

#ifndef TEDGE_PROVISIONING_INTERNAL_H_
#define TEDGE_PROVISIONING_INTERNAL_H_

#include <tedge/provisioning.h>

#include <stddef.h>
#include <stdint.h>

/* Improv's states, errors and commands (https://www.improv-wifi.com). */
#define IMPROV_STATE_READY        0x02 /* "authorized" on BLE */
#define IMPROV_STATE_PROVISIONING 0x03
#define IMPROV_STATE_PROVISIONED  0x04

#define IMPROV_ERR_NONE           0x00
#define IMPROV_ERR_INVALID_RPC    0x01
#define IMPROV_ERR_UNKNOWN_RPC    0x02
#define IMPROV_ERR_UNABLE_CONNECT 0x03
#define IMPROV_ERR_NOT_AUTHORIZED 0x04
#define IMPROV_ERR_UNKNOWN        0xFF

#define IMPROV_CMD_WIFI_SETTINGS  0x01
#define IMPROV_CMD_CURRENT_STATE  0x02 /* serial; "identify" on BLE */
#define IMPROV_CMD_DEVICE_INFO    0x03

/* An RPC packet as the transports hand it over: [command][length][data].
 * The largest the library accepts: the onboard command with a 64-byte host
 * and a 64-byte password. */
#define RPC_MAX 160

/* How the RPC layer answers, whatever the transport. */
struct rpc_out {
	void (*state)(uint8_t state);
	void (*error)(uint8_t error);
	/* An RPC result: [command][length][strings, each length-prefixed]. */
	void (*result)(const uint8_t *data, size_t len);
};

/* Queue one RPC packet from @p out's transport, for the worker thread.
 * @return 0, or -ENOMEM when the queue is full. */
int tedge_prov_rpc_submit(const uint8_t *pkt, size_t len,
			  const struct rpc_out *out);

/* The serial transport's answers (tedge_improv_serial.c). */
extern const struct rpc_out tedge_prov_serial_out;

#endif /* TEDGE_PROVISIONING_INTERNAL_H_ */
