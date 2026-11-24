/* SPDX-License-Identifier: Apache-2.0
 *
 * Which roots a TLS connection trusts (ports/common/src/tedge_sockets.c):
 * a loopback mbedTLS server presents a certificate from a download-only
 * root, and only TEDGE_PORT_TLS_DOWNLOAD connections, made after that root
 * was given to tedge_port_tls_set_extra_ca(), accept it.
 */

#include <tedge/port/net.h>

#include "test_tls_certs.h"

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__,     \
				#cond);                                        \
			failures++;                                            \
		}                                                              \
	} while (0)

#include "test_tls_server.h"

/* The roots TEDGE_DOWNLOAD_CA_FILES embeds, as the core does. */
static const unsigned char default_download_ca[] = {
#include "tedge_download_ca.inc"
	0x00
};

static void test_default_bundle(void)
{
	mbedtls_x509_crt crt;
	int n = 0;

	if (sizeof(default_download_ca) == 1) {
		return; /* built with TEDGE_DOWNLOAD_CA_FILES empty */
	}
	mbedtls_x509_crt_init(&crt);
	/* 0, not a count of certificates skipped: every one parses, and the
	 * comment lines in the PEM files are ignored. */
	CHECK(mbedtls_x509_crt_parse(&crt, default_download_ca,
				     sizeof(default_download_ca)) == 0);
	for (const mbedtls_x509_crt *c = &crt; c != NULL && c->raw.len > 0;
	     c = c->next) {
		n++;
	}
	CHECK(n == 2); /* ISRG Root X1, USERTrust ECC */
	mbedtls_x509_crt_free(&crt);
}

int main(void)
{
	static const struct tedge_port_ca_buf dl[] = {
		{ dl_ca, sizeof(dl_ca) },
	};
	static const unsigned char junk[] = "not a certificate";
	static const struct tedge_port_ca_buf junk_then_dl[] = {
		{ junk, sizeof(junk) },
		{ dl_ca, sizeof(dl_ca) },
	};
	if (psa_crypto_init() != PSA_SUCCESS || server_start() != 0) {
		return 1;
	}
	test_default_bundle();

	/* The tenant's root alone: no connection trusts the server. */
	CHECK(tedge_port_tls_set_ca(tenant_ca, sizeof(tenant_ca)) == 0);
	CHECK(try_connect(TEDGE_PORT_TLS) == -EACCES);
	CHECK(try_connect(TEDGE_PORT_TLS_DOWNLOAD) == -EACCES);

	/* With the download root: downloads trust it, the tenant scope
	 * still does not. */
	CHECK(tedge_port_tls_set_extra_ca(TEDGE_PORT_CA_DOWNLOAD, dl, 1) == 0);
	CHECK(try_connect(TEDGE_PORT_TLS_DOWNLOAD) == 0);
	CHECK(try_connect(TEDGE_PORT_TLS) == -EACCES);

	/* A buffer that does not parse is skipped, not fatal. */
	CHECK(tedge_port_tls_set_extra_ca(TEDGE_PORT_CA_DOWNLOAD, junk_then_dl,
					  2) == 0);
	CHECK(try_connect(TEDGE_PORT_TLS_DOWNLOAD) == 0);

	/* Cleared: back to the tenant's root alone. */
	CHECK(tedge_port_tls_set_extra_ca(TEDGE_PORT_CA_DOWNLOAD, NULL, 0) == 0);
	CHECK(try_connect(TEDGE_PORT_TLS_DOWNLOAD) == -EACCES);

	/* Bad arguments. */
	CHECK(tedge_port_tls_set_extra_ca(TEDGE_PORT_CA_DOWNLOAD, NULL, 1) ==
	      -EINVAL);

	server_stop();
	if (failures == 0) {
		printf("all TLS trust checks passed\n");
	}
	return failures == 0 ? 0 : 1;
}
