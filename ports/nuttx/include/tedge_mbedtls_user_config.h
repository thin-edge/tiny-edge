/* SPDX-License-Identifier: Apache-2.0
 *
 * mbedTLS 4 (TF-PSA-Crypto) settings on top of its defaults, as the POSIX
 * port sets them: the client's threads share the PSA key store.
 */

#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
