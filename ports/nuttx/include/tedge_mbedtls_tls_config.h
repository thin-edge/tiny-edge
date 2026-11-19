/* SPDX-License-Identifier: Apache-2.0
 *
 * mbedTLS 4 TLS settings on top of its defaults (MBEDTLS_USER_CONFIG_FILE),
 * as the Zephyr port sets them (ports/zephyr/include/tedge/
 * mbedtls_user_config.h, which explains them in full).
 */

/* The client only sends small records (MQTT publishes, HTTP request heads,
 * upload chunks, WebSocket frames); a larger write is split into several
 * records. Only received records need the 16 KB the server may use. This
 * saves 12 KB of heap for each TLS session. */
#undef MBEDTLS_SSL_OUT_CONTENT_LEN
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

/* A TLS client only: the server role's handshake is never used. */
#undef MBEDTLS_SSL_SRV_C
