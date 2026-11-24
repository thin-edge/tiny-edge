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

#define HOST "download.test"

static int listen_fd;
static volatile bool stopping;

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	ssize_t n = send(*(int *)ctx, buf, len, 0);

	return (n < 0) ? MBEDTLS_ERR_NET_SEND_FAILED : (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	ssize_t n = recv(*(int *)ctx, buf, len, 0);

	return (n < 0) ? MBEDTLS_ERR_NET_RECV_FAILED : (int)n;
}

/* Accepts connections one at a time and shakes hands with each, until the
 * test is done. Whether the client accepted is the client's to report. */
static void *server(void *arg)
{
	mbedtls_x509_crt cert;
	mbedtls_pk_context key;
	mbedtls_ssl_config conf;

	(void)arg;
	mbedtls_x509_crt_init(&cert);
	mbedtls_pk_init(&key);
	mbedtls_ssl_config_init(&conf);
	if (mbedtls_x509_crt_parse(&cert, (const unsigned char *)srv_cert,
				   sizeof(srv_cert)) != 0 ||
	    mbedtls_pk_parse_key(&key, (const unsigned char *)srv_key,
				 sizeof(srv_key), NULL, 0) != 0 ||
	    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER,
					MBEDTLS_SSL_TRANSPORT_STREAM,
					MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
	    mbedtls_ssl_conf_own_cert(&conf, &cert, &key) != 0) {
		fprintf(stderr, "test server: setup failed\n");
		failures++;
		return NULL;
	}
	while (!stopping) {
		int fd = accept(listen_fd, NULL, NULL);
		mbedtls_ssl_context ssl;

		if (fd < 0) {
			continue;
		}
		mbedtls_ssl_init(&ssl);
		if (mbedtls_ssl_setup(&ssl, &conf) == 0) {
			mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, NULL);
			(void)mbedtls_ssl_handshake(&ssl);
		}
		mbedtls_ssl_free(&ssl);
		(void)close(fd);
	}
	mbedtls_ssl_config_free(&conf);
	mbedtls_pk_free(&key);
	mbedtls_x509_crt_free(&cert);
	return NULL;
}

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

static const uint8_t lo[4] = { 127, 0, 0, 1 };
static uint16_t port;

/* Open a connection, and report whether the handshake was accepted. */
static int try_connect(enum tedge_port_security security)
{
	int conn = tedge_port_connect(lo, port, security, HOST, 5000);

	if (conn >= 0) {
		tedge_port_close(conn);
		return 0;
	}
	return conn;
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
	struct sockaddr_in sa = { .sin_family = AF_INET };
	socklen_t len = sizeof(sa);
	pthread_t thread;

	if (psa_crypto_init() != PSA_SUCCESS) {
		fprintf(stderr, "PSA did not start\n");
		return 1;
	}
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(listen_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
	    listen(listen_fd, 4) != 0 ||
	    getsockname(listen_fd, (struct sockaddr *)&sa, &len) != 0) {
		perror("listen");
		return 1;
	}
	port = ntohs(sa.sin_port);
	test_default_bundle();
	(void)pthread_create(&thread, NULL, server, NULL);

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

	stopping = true;
	(void)shutdown(listen_fd, SHUT_RDWR);
	{
		/* Wake accept() on systems where shutdown() does not. */
		int fd = socket(AF_INET, SOCK_STREAM, 0);

		(void)connect(fd, (struct sockaddr *)&sa, sizeof(sa));
		(void)close(fd);
	}
	(void)pthread_join(thread, NULL);
	(void)close(listen_fd);
	if (failures == 0) {
		printf("all TLS trust checks passed\n");
	}
	return failures == 0 ? 0 : 1;
}
