/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief The device's key and random numbers: the port interface.
 *
 * The device's private key never leaves the port. It lives in the
 * platform's key store (PSA on Zephyr) and the port uses it for the two
 * things that need it: signing a certificate request, and the TLS
 * handshake (tedge_port_tls_set_device() in net.h pairs it with the
 * certificate the core enrolled).
 *
 * Parsing and encoding that need no key (X.509, ASN.1, base64) the core
 * does itself, with mbedTLS's parsers or its own code.
 */

#ifndef TEDGE_PORT_CRYPTO_H_
#define TEDGE_PORT_CRYPTO_H_

#include <stddef.h>

/**
 * Make sure the device has its key pair (ECDSA P-256), generating and
 * storing it on the first call ever; later calls, after any number of
 * reboots, find the same key.
 *
 * @return 0, or a negative errno.
 */
int tedge_port_device_key_ensure(void);

/**
 * A PKCS#10 certificate request for the device key, signed with it
 * (ECDSA with SHA-256), with the subject @p subject ("CN=<id>").
 *
 * @param pem Out: the request in PEM, NUL-terminated.
 * @return The length of @p pem, or a negative errno.
 */
int tedge_port_device_csr(const char *subject, char *pem, size_t cap);

/** Fill @p buf with @p len random bytes from a cryptographic source.
 *  @return 0, or a negative errno. */
int tedge_port_random(void *buf, size_t len);

#endif /* TEDGE_PORT_CRYPTO_H_ */
