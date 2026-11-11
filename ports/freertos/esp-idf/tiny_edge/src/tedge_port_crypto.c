/* SPDX-License-Identifier: Apache-2.0
 *
 * Where the ESP-IDF port keeps the device key: NVS, through the client's
 * own store (tedge_port_kv.c), as PEM. Generating it and signing the CSR
 * are shared with other ports (ports/common/src/tedge_mbedtls_key.c).
 *
 * NVS is plain flash unless the application turns on NVS encryption
 * (CONFIG_NVS_ENCRYPTION, with flash encryption), which a production
 * device should: the key is only as safe as the flash it is in.
 */

#include <tedge/port/kv.h>

#include "tedge_common.h"

#define KEY "tedge/device-key"

const char *tedge_common_key_where(void)
{
	return "NVS " KEY;
}

int tedge_common_key_load(char *pem, size_t cap)
{
	return tedge_port_kv_get(KEY, pem, cap);
}

int tedge_common_key_store(const char *pem, size_t len)
{
	return tedge_port_kv_set(KEY, pem, len);
}
