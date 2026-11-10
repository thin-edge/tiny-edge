/* SPDX-License-Identifier: Apache-2.0 */

/* The POSIX port's mbedTLS glue, shared by its crypto and network sources;
 * not part of the port interface. */

#ifndef TEDGE_POSIX_TLS_H_
#define TEDGE_POSIX_TLS_H_

#include <mbedtls/pk.h>

/* Several client threads hold TLS sessions at once, and PSA's key slots
 * and random generator are shared between them. */
#if !defined(MBEDTLS_THREADING_C) || !defined(MBEDTLS_THREADING_PTHREAD)
#error "tiny-edge needs mbedTLS built with MBEDTLS_THREADING_C and MBEDTLS_THREADING_PTHREAD"
#endif

/** Start PSA crypto, once. @return 0, or -EIO. */
int tedge_posix_psa_init(void);

/** Load the device key into @p pk (initialised by the caller).
 *  @return 0; -ENOENT when there is no key yet; or another negative errno. */
int tedge_posix_device_pk(mbedtls_pk_context *pk);

#endif /* TEDGE_POSIX_TLS_H_ */
