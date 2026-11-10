/* SPDX-License-Identifier: Apache-2.0 */

/* Added to mbedTLS's default configuration when the POSIX port builds
 * mbedTLS itself (TF_PSA_CRYPTO_USER_CONFIG_FILE): the client holds TLS
 * sessions on several threads, which share PSA's key slots and random
 * generator. */

#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
