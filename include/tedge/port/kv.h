/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief Persistent key-value storage: the port interface.
 *
 * What the client must remember across a reset: the tenant, credentials,
 * parameter values, and the state of a firmware update in progress. The
 * port keeps it where the RTOS keeps such things (Zephyr's settings
 * subsystem, ESP-IDF's NVS, a file).
 *
 * Keys are strings with "/" between levels, all under "tedge/". Values
 * are opaque bytes. A firmware update must not lose them, and the format
 * of a key's value is a contract between firmware versions: the image on
 * either side of an update reads what the other wrote.
 *
 * Calls come from the client's threads, never from an interrupt, and may
 * take as long as a flash write takes.
 */

#ifndef TEDGE_PORT_KV_H_
#define TEDGE_PORT_KV_H_

#include <stddef.h>

/** Make the store ready. Called once, from tedge_init(), before any other
 *  call. @return 0, or a negative errno. */
int tedge_port_kv_init(void);

/** Store @p len bytes of @p value under @p key, replacing what was there.
 *  @return 0, or a negative errno. */
int tedge_port_kv_set(const char *key, const void *value, size_t len);

/**
 * Read the value stored under exactly @p key into @p buf.
 *
 * @return The length of the value, which is what was copied; -ENOENT when
 *         nothing is stored; -ENOSPC when the value is longer than @p cap
 *         (nothing useful is copied then); or another negative errno.
 */
int tedge_port_kv_get(const char *key, void *buf, size_t cap);

/** Forget @p key. Forgetting a key that is not stored is not an error.
 *  @return 0, or a negative errno. */
int tedge_port_kv_delete(const char *key);

/**
 * Call @p cb once for every key stored under @p prefix + "/", with the
 * rest of the key ("<set>/<name>" under "tedge/param"). @p cb must not
 * change the store; collect the keys and act on them afterwards.
 *
 * @return 0, or a negative errno.
 */
int tedge_port_kv_foreach(const char *prefix,
			  void (*cb)(const char *subkey, void *ctx), void *ctx);

#endif /* TEDGE_PORT_KV_H_ */
