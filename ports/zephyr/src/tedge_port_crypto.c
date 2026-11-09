/* SPDX-License-Identifier: Apache-2.0
 *
 * The Zephyr side of <tedge/port/crypto.h>: the device key in PSA
 * persistent storage (CONFIG_TEDGE_PSA_KEY_ID), a CSR signed through it
 * with mbedTLS, and the device's TLS credential.
 *
 * Zephyr's TLS credentials take the key as a buffer, so it is exported
 * once, as DER, when the certificate is registered; that copy stays in RAM
 * while the credential is registered (a known limitation, spikes P9).
 */

#include <tedge/port/crypto.h>
#include <tedge/port/net.h>

#include "tedge_port_zephyr.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/tls_credentials.h>

#include <psa/crypto.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_csr.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

#define KEY_ID ((psa_key_id_t)CONFIG_TEDGE_PSA_KEY_ID)

/* Kept while the credential is registered. */
static uint8_t key_der_buf[160];

int tedge_port_device_key_ensure(void)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id;
	psa_status_t st;

	st = psa_get_key_attributes(KEY_ID, &attr);
	psa_reset_key_attributes(&attr);
	if (st == PSA_SUCCESS) {
		return 0; /* already generated on an earlier boot */
	}

	psa_set_key_id(&attr, KEY_ID);
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_PERSISTENT);
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH |
					       PSA_KEY_USAGE_SIGN_MESSAGE |
					       PSA_KEY_USAGE_EXPORT);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	st = psa_generate_key(&attr, &id);
	psa_reset_key_attributes(&attr);
	if (st != PSA_SUCCESS) {
		LOG_ERR("generating the device key failed (%d)", (int)st);
		return -EIO;
	}
	LOG_INF("device key generated (P-256, PSA key 0x%08x)",
		(unsigned int)KEY_ID);
	return 0;
}

int tedge_port_device_csr(const char *subject, char *pem, size_t cap)
{
	mbedtls_x509write_csr csr;
	mbedtls_pk_context pk;
	int ret;

	mbedtls_x509write_csr_init(&csr);
	mbedtls_pk_init(&pk);

	ret = mbedtls_pk_wrap_psa(&pk, KEY_ID);
	if (ret != 0) {
		LOG_ERR("could not use the PSA key for the CSR (-0x%04x)", -ret);
		ret = -EIO;
		goto out;
	}
	mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
	mbedtls_x509write_csr_set_key(&csr, &pk);
	ret = mbedtls_x509write_csr_set_subject_name(&csr, subject);
	if (ret == 0) {
		ret = mbedtls_x509write_csr_pem(&csr, (unsigned char *)pem, cap);
	}
	if (ret != 0) {
		LOG_ERR("writing the CSR failed (-0x%04x)", -ret);
		ret = -EIO;
		goto out;
	}
	ret = (int)strlen(pem);
out:
	mbedtls_pk_free(&pk);
	mbedtls_x509write_csr_free(&csr);
	return ret;
}

int tedge_port_random(void *buf, size_t len)
{
	return (psa_generate_random(buf, len) == PSA_SUCCESS) ? 0 : -EIO;
}

void tedge_port_tls_clear_device(void)
{
	(void)tls_credential_delete(TAG_DEVICE,
				    TLS_CREDENTIAL_PUBLIC_CERTIFICATE);
	(void)tls_credential_delete(TAG_DEVICE, TLS_CREDENTIAL_PRIVATE_KEY);
}

int tedge_port_tls_set_device(const void *cert, size_t cert_len)
{
	mbedtls_pk_context pk;
	const uint8_t *key_der;
	int ret;

	mbedtls_pk_init(&pk);
	ret = mbedtls_pk_copy_from_psa(KEY_ID, &pk);
	if (ret == 0) {
		ret = mbedtls_pk_write_key_der(&pk, key_der_buf,
					       sizeof(key_der_buf));
	}
	mbedtls_pk_free(&pk);
	if (ret <= 0) {
		LOG_ERR("exporting the device key failed (-0x%04x)", -ret);
		return -EIO;
	}
	/* mbedtls_pk_write_key_der writes at the end of the buffer. */
	key_der = key_der_buf + sizeof(key_der_buf) - ret;

	tedge_port_tls_clear_device();
	ret = tls_credential_add(TAG_DEVICE, TLS_CREDENTIAL_PUBLIC_CERTIFICATE,
				 cert, cert_len);
	if (ret == 0) {
		ret = tls_credential_add(TAG_DEVICE, TLS_CREDENTIAL_PRIVATE_KEY,
					 key_der,
					 sizeof(key_der_buf) -
						 (size_t)(key_der - key_der_buf));
	}
	return ret;
}
