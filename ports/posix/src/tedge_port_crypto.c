/* SPDX-License-Identifier: Apache-2.0
 *
 * The POSIX side of <tedge/port/crypto.h>: the device key as a PEM file in
 * the state directory, readable by the owner only, as thin-edge.io keeps
 * its keys on Linux (device-key.pem); a CSR signed with it by mbedTLS; and
 * the system's random numbers.
 */

#include <tedge/port/crypto.h>
#include <tedge/port/log.h>

#include "tedge_posix_internal.h"
#include "tedge_posix_tls.h"

#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/x509_csr.h>
#include <psa/crypto.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h> /* getentropy() on glibc */

TEDGE_LOG_DECLARE();

#define KEY_FILE "device-key.pem"
/* A P-256 key in PEM is about 230 bytes. */
#define KEY_PEM_MAX 512

static pthread_once_t psa_once = PTHREAD_ONCE_INIT;
static psa_status_t psa_init_status;

static void psa_init(void)
{
	psa_init_status = psa_crypto_init();
}

int tedge_posix_psa_init(void)
{
	(void)pthread_once(&psa_once, psa_init);
	if (psa_init_status != PSA_SUCCESS) {
		TEDGE_LOG_ERR("PSA crypto did not start (%d)", (int)psa_init_status);
		return -EIO;
	}
	return 0;
}

static int key_path(char *buf, size_t len)
{
	return tedge_posix_path(buf, len, KEY_FILE);
}

int tedge_posix_device_pk(mbedtls_pk_context *pk)
{
	unsigned char pem[KEY_PEM_MAX];
	char path[PATH_MAX];
	ssize_t n;
	int ret = tedge_posix_psa_init();

	if (ret == 0) {
		ret = key_path(path, sizeof(path));
	}
	if (ret != 0) {
		return ret;
	}
	n = tedge_posix_read_file(path, pem, sizeof(pem) - 1);
	if (n < 0) {
		return (int)n;
	}
	if ((size_t)n >= sizeof(pem)) {
		return -EFBIG;
	}
	pem[n] = '\0';
	/* PEM is parsed with its NUL. */
	ret = mbedtls_pk_parse_key(pk, pem, (size_t)n + 1, NULL, 0);
	mbedtls_platform_zeroize(pem, sizeof(pem));
	if (ret != 0) {
		TEDGE_LOG_ERR("the device key (%s) is not usable (-0x%04x)", path,
			      (unsigned int)-ret);
		return -EINVAL;
	}
	return 0;
}

int tedge_port_device_key_ensure(void)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	unsigned char pem[KEY_PEM_MAX];
	mbedtls_pk_context pk;
	char path[PATH_MAX];
	psa_key_id_t id = 0;
	psa_status_t st;
	int ret;

	mbedtls_pk_init(&pk);
	ret = tedge_posix_device_pk(&pk);
	mbedtls_pk_free(&pk);
	if (ret != -ENOENT) {
		return ret; /* 0: generated on an earlier start */
	}

	/* Generated in PSA, written out once as PEM, then forgotten by PSA:
	 * the file is the key's only home. */
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
	ret = key_path(path, sizeof(path));
	if (ret == 0) {
		ret = tedge_posix_write_file(path, pem, strlen((char *)pem), 0600);
	}
	mbedtls_platform_zeroize(pem, sizeof(pem));
	if (ret != 0) {
		TEDGE_LOG_ERR("could not store the device key in %s (%d)", path, ret);
		return ret;
	}
	TEDGE_LOG_INF("device key generated (P-256, %s)", path);
	return 0;
}

int tedge_port_device_csr(const char *subject, char *pem, size_t cap)
{
	mbedtls_x509write_csr csr;
	mbedtls_pk_context pk;
	int ret;

	mbedtls_x509write_csr_init(&csr);
	mbedtls_pk_init(&pk);
	ret = tedge_posix_device_pk(&pk);
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

int tedge_port_random(void *buf, size_t len)
{
	uint8_t *p = buf;

	/* getentropy() gives at most 256 bytes a call. */
	while (len > 0) {
		size_t n = (len > 256) ? 256 : len;

		if (getentropy(p, n) != 0) {
			return -errno;
		}
		p += n;
		len -= n;
	}
	return 0;
}
