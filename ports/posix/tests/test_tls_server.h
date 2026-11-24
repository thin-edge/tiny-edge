/* SPDX-License-Identifier: Apache-2.0
 *
 * A loopback TLS server for the port's trust tests: it presents srv_cert
 * (test_tls_certs.h, issued by dl_ca for "download.test") and shakes hands
 * with each connection. The test that includes it defines `failures`.
 */

#ifndef TEST_TLS_SERVER_H_
#define TEST_TLS_SERVER_H_

#include <tedge/port/net.h>

#include "test_tls_certs.h"

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#define HOST "download.test"

static int listen_fd;
static struct sockaddr_in server_sa;
static pthread_t server_thread;
static volatile bool stopping;

/* A client that refuses the certificate hangs up mid-handshake: writing to
 * it must fail, not raise SIGPIPE (Linux), which would end the test. */
#if defined(MSG_NOSIGNAL)
#define TEST_SEND_FLAGS MSG_NOSIGNAL
#else
#define TEST_SEND_FLAGS 0
#endif

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	ssize_t n = send(*(int *)ctx, buf, len, TEST_SEND_FLAGS);

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

static const uint8_t lo[4] = { 127, 0, 0, 1 };

/* Start listening on a free loopback port. @return 0, or -1. */
static int server_start(void)
{
	socklen_t len = sizeof(server_sa);

	/* macOS has no MSG_NOSIGNAL: ignore the signal for the whole test. */
	(void)signal(SIGPIPE, SIG_IGN);

	server_sa.sin_family = AF_INET;
	server_sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (bind(listen_fd, (struct sockaddr *)&server_sa,
		 sizeof(server_sa)) != 0 ||
	    listen(listen_fd, 4) != 0 ||
	    getsockname(listen_fd, (struct sockaddr *)&server_sa, &len) != 0) {
		perror("listen");
		return -1;
	}
	return pthread_create(&server_thread, NULL, server, NULL) == 0 ? 0 : -1;
}

static void server_stop(void)
{
	int fd;

	stopping = true;
	(void)shutdown(listen_fd, SHUT_RDWR);
	/* Wake accept() on systems where shutdown() does not. */
	fd = socket(AF_INET, SOCK_STREAM, 0);
	(void)connect(fd, (struct sockaddr *)&server_sa, sizeof(server_sa));
	(void)close(fd);
	(void)pthread_join(server_thread, NULL);
	(void)close(listen_fd);
}

/* Open a connection to the server: 0 when the handshake was accepted, or
 * the port's negative errno (-EACCES: the certificate is not trusted). */
static int try_connect(enum tedge_port_security security)
{
	int conn = tedge_port_connect(lo, ntohs(server_sa.sin_port), security,
				      HOST, 5000);

	if (conn >= 0) {
		tedge_port_close(conn);
		return 0;
	}
	return conn;
}

#endif /* TEST_TLS_SERVER_H_ */
