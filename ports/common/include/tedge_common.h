/* SPDX-License-Identifier: Apache-2.0 */

/* What the ports share: code for platforms that offer BSD sockets and
 * mbedTLS 4 (POSIX, and ESP-IDF's lwIP), and what it needs from each
 * platform. Not part of the port interface, nor of the public API.
 *
 * - tedge_sockets.c implements the connections and TLS of
 *   <tedge/port/net.h>: everything but the interface queries
 *   (tedge_port_net_is_up, _watch, _is_own, _on_subnet), which differ by
 *   platform.
 * - tedge_mbedtls_key.c implements <tedge/port/crypto.h> but
 *   tedge_port_random(): the device key, generated with PSA and kept as
 *   PEM where the platform keeps it, and the CSR signed with it.
 */

#ifndef TEDGE_COMMON_H_
#define TEDGE_COMMON_H_

#include <mbedtls/pk.h>

#include <stddef.h>

/* Several client threads hold TLS sessions at once, and PSA's key slots
 * and random generator are shared between them. */
#if !defined(MBEDTLS_THREADING_C)
#error "tiny-edge needs mbedTLS built with MBEDTLS_THREADING_C"
#endif

/* --- Provided by the shared code ----------------------------------------- */

/** Start PSA crypto, once (tedge_sockets.c). @return 0, or -EIO. */
int tedge_common_psa_init(void);

/** Load the device key into @p pk (initialised by the caller); only in
 *  builds with enrollment (tedge_mbedtls_key.c).
 *  @return 0; -ENOENT when there is no key yet; or another negative errno. */
int tedge_common_device_pk(mbedtls_pk_context *pk);

/* --- Provided by the platform -------------------------------------------- */

/**
 * Read the stored device key, PEM, into @p pem.
 *
 * @return Its length (which may be more than @p cap: then it did not fit);
 *         -ENOENT when there is none; or another negative errno.
 */
int tedge_common_key_load(char *pem, size_t cap);

/** Store the device key, @p len bytes of PEM, readable by the device only.
 *  @return 0, or a negative errno. */
int tedge_common_key_store(const char *pem, size_t len);

/** Where the key is kept, for log lines ("/var/lib/x/device-key.pem"). */
const char *tedge_common_key_where(void);

/** A socket was opened for the client: set what the platform needs on
 *  every one (close-on-exec, no SIGPIPE). It is non-blocking already. */
void tedge_common_socket_opened(int fd);

#endif /* TEDGE_COMMON_H_ */
