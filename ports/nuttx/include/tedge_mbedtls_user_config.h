/* SPDX-License-Identifier: Apache-2.0
 *
 * mbedTLS 4 crypto (TF-PSA-Crypto) settings on top of its defaults
 * (TF_PSA_CRYPTO_USER_CONFIG_FILE).
 */

/* The client's threads share the PSA key store, as on the POSIX port. */
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD

/* Without this, every PSA call copies its input and its output into
 * buffers of its own on the heap: for each TLS record that is encrypted or
 * decrypted, up to two 16 KB copies. With a firmware download and the MQTT
 * session both busy, those copies ran the heap out, and TLS failed with
 * PSA_ERROR_INSUFFICIENT_MEMORY on both connections (thin-edge/tiny-edge#32).
 * The copies only protect buffers shared with an untrusted party; here every
 * buffer belongs to the one application, as on ESP-IDF, whose mbedTLS
 * configuration sets this too. */
#define MBEDTLS_PSA_ASSUME_EXCLUSIVE_BUFFERS
