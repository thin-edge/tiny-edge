/* SPDX-License-Identifier: Apache-2.0
 *
 * The device key of <tedge/port/crypto.h> with mbedTLS 4 and PSA, for any
 * platform that keeps the key as PEM in its own storage (a file on POSIX,
 * NVS on ESP-IDF: tedge_common_key_load() and _store()). The key is
 * generated once in PSA, written out, and parsed again for each use.
 *
 * The platform adds tedge_port_random().
 */

#include <tedge/port/crypto.h>
#include <tedge/port/log.h>
#include <tedge/port/os.h>

#include "tedge_common.h"

#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/x509_csr.h>
#include <psa/crypto.h>

#include <errno.h>
#include <string.h>

TEDGE_LOG_DECLARE();

/* A P-256 key in PEM is about 230 bytes. */
#define KEY_PEM_MAX 512

int tedge_common_device_pk(mbedtls_pk_context *pk)
{
	unsigned char pem[KEY_PEM_MAX];
	int n, ret = tedge_common_psa_init();

	if (ret != 0) {
		return ret;
	}
	n = tedge_common_key_load((char *)pem, sizeof(pem) - 1);
	if (n < 0) {
		return n;
	}
	if ((size_t)n >= sizeof(pem)) {
		return -EFBIG;
	}
	pem[n] = '\0';
	/* PEM is parsed with its NUL. */
	ret = mbedtls_pk_parse_key(pk, pem, (size_t)n + 1, NULL, 0);
	mbedtls_platform_zeroize(pem, sizeof(pem));
	if (ret != 0) {
		TEDGE_LOG_ERR("the device key (%s) is not usable (-0x%04x)",
			      tedge_common_key_where(), (unsigned int)-ret);
		return -EINVAL;
	}
	return 0;
}

int tedge_port_device_key_ensure(void)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	unsigned char pem[KEY_PEM_MAX];
	mbedtls_pk_context pk;
	psa_key_id_t id = 0;
	psa_status_t st;
	int ret;

	mbedtls_pk_init(&pk);
	ret = tedge_common_device_pk(&pk);
	mbedtls_pk_free(&pk);
	if (ret != -ENOENT) {
		return ret; /* 0: generated on an earlier start */
	}

	/* Generated in PSA, written out once as PEM, then forgotten by PSA:
	 * the platform's storage is the key's only home. */
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH |
					       PSA_KEY_USAGE_SIGN_MESSAGE |
					       PSA_KEY_USAGE_EXPORT);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	st = psa_generate_key(&attr, &id);
	psa_reset_key_attributes(&attr);
	if (st != PSA_SUCCESS) {
		TEDGE_LOG_ERR("generating the device key failed (%d)", (int)st);
		return -EIO;
	}
	mbedtls_pk_init(&pk);
	ret = mbedtls_pk_copy_from_psa(id, &pk);
	if (ret == 0) {
		ret = mbedtls_pk_write_key_pem(&pk, pem, sizeof(pem));
	}
	mbedtls_pk_free(&pk);
	(void)psa_destroy_key(id);
	if (ret != 0) {
		TEDGE_LOG_ERR("exporting the device key failed (-0x%04x)",
			      (unsigned int)-ret);
		return -EIO;
	}
	ret = tedge_common_key_store((const char *)pem, strlen((char *)pem));
	mbedtls_platform_zeroize(pem, sizeof(pem));
	if (ret != 0) {
		TEDGE_LOG_ERR("could not store the device key in %s (%d)",
			      tedge_common_key_where(), ret);
		return ret;
	}
	TEDGE_LOG_INF("device key generated (P-256, %s)", tedge_common_key_where());
	return 0;
}

int tedge_port_device_csr(const char *subject, char *pem, size_t cap)
{
	mbedtls_x509write_csr csr;
	mbedtls_pk_context pk;
	int ret;

	mbedtls_x509write_csr_init(&csr);
	mbedtls_pk_init(&pk);
	ret = tedge_common_device_pk(&pk);
	if (ret != 0) {
		goto out;
	}
	mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
	mbedtls_x509write_csr_set_key(&csr, &pk);
	ret = mbedtls_x509write_csr_set_subject_name(&csr, subject);
	if (ret == 0) {
		ret = mbedtls_x509write_csr_pem(&csr, (unsigned char *)pem, cap);
	}
	if (ret != 0) {
		TEDGE_LOG_ERR("writing the CSR failed (-0x%04x)", (unsigned int)-ret);
		ret = -EIO;
		goto out;
	}
	ret = (int)strlen(pem);
out:
	mbedtls_pk_free(&pk);
	mbedtls_x509write_csr_free(&csr);
	return ret;
}
