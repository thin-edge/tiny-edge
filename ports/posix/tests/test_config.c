/* SPDX-License-Identifier: Apache-2.0
 *
 * Configuration management (core/src/tedge_config_mgmt.c) against a fake
 * Cumulocity tenant: an HTTPS server on loopback that answers the device's
 * identity lookup, creates its event, takes the binary, and serves the files
 * a set downloads. It checks what the device sends and what it reports for
 * each request: a get, a set, and the ways a set can fail.
 */

#include "tedge_internal.h"

#include <tedge/port/kv.h>
#include <tedge/port/net.h>
#include <tedge/posix.h>

#include <psa/crypto.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__,     \
				#cond);                                        \
			failures++;                                            \
		}                                                              \
	} while (0)

#include "test_tls_certs.h"
#include "test_trust_certs.h"

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

/* ------------------------------------------------------------------------ */
/* The fake tenant                                                           */
/* ------------------------------------------------------------------------ */

static int listen_fd;
static struct sockaddr_in server_sa;
static pthread_t server_thread;
static volatile bool stopping;
static char host[32]; /* "localhost:<port>" */

/* What the device sent, for the checks. */
static char event_body[512];
static char binary_body[8192];
static char binary_headers[1024];

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

static void respond(mbedtls_ssl_context *ssl, int status, const char *extra,
		    const char *body, size_t len)
{
	char head[512];
	int n = snprintf(head, sizeof(head),
			 "HTTP/1.1 %d X\r\nContent-Length: %zu\r\n%s"
			 "Connection: close\r\n\r\n",
			 status, len, (extra != NULL) ? extra : "");

	(void)mbedtls_ssl_write(ssl, (const unsigned char *)head, (size_t)n);
	for (size_t off = 0; off < len;) {
		int w = mbedtls_ssl_write(ssl, (const unsigned char *)body + off,
					  len - off);

		if (w <= 0) {
			break;
		}
		off += (size_t)w;
	}
}

/* 20 KB of comment text: more than TEDGE_CONFIG_MAX_BYTES. */
static char big_file[20 * 1024];

static void serve(mbedtls_ssl_context *ssl)
{
	static char req[16384];
	size_t used = 0, need = 0;
	char *body = NULL;
	char method[8] = "", path[256] = "";

	/* The head, then as much body as Content-Length says. */
	for (;;) {
		int n = mbedtls_ssl_read(ssl, (unsigned char *)req + used,
					 sizeof(req) - 1 - used);

		if (n <= 0) {
			return;
		}
		used += (size_t)n;
		req[used] = '\0';
		if (body == NULL && (body = strstr(req, "\r\n\r\n")) != NULL) {
			const char *cl = strstr(req, "Content-Length: ");

			body += 4;
			need = (size_t)(body - req) +
			       ((cl != NULL) ? strtoul(cl + 16, NULL, 10) : 0);
		}
		if (body != NULL && used >= need) {
			break;
		}
	}
	(void)sscanf(req, "%7s %255s", method, path);

	if (strcmp(method, "GET") == 0 &&
	    strncmp(path, "/identity/externalIds/c8y_Serial/", 33) == 0) {
		static const char ans[] = "{\"externalId\":\"x\",\"managedObject\""
					  ":{\"id\":\"4242\"}}";

		respond(ssl, 200, "Content-Type: application/json\r\n", ans,
			strlen(ans));
	} else if (strcmp(method, "POST") == 0 &&
		   strcmp(path, "/event/events") == 0) {
		char loc[128];
		static const char ans[] = "{\"id\":\"77\"}";

		snprintf(event_body, sizeof(event_body), "%s", body);
		snprintf(loc, sizeof(loc),
			 "Location: https://%s/event/events/77\r\n", host);
		respond(ssl, 201, loc, ans, strlen(ans));
	} else if (strcmp(method, "POST") == 0 &&
		   strcmp(path, "/event/events/77/binaries") == 0) {
		snprintf(binary_headers, sizeof(binary_headers), "%.*s",
			 (int)(body - req), req);
		snprintf(binary_body, sizeof(binary_body), "%s", body);
		respond(ssl, 201, NULL, "{}", 2);
	} else if (strcmp(path, "/roots/good.pem") == 0) {
		char file[2048];

		snprintf(file, sizeof(file), "# two roots\n%s%s", ca1, ca2);
		respond(ssl, 200, NULL, file, strlen(file));
	} else if (strcmp(path, "/roots/leaf.pem") == 0) {
		respond(ssl, 200, NULL, srv_cert, strlen(srv_cert));
	} else if (strcmp(path, "/roots/big.pem") == 0) {
		respond(ssl, 200, NULL, big_file, sizeof(big_file));
	} else {
		respond(ssl, 404, NULL, "", 0);
	}
}

static void *server(void *arg)
{
	mbedtls_x509_crt cert;
	mbedtls_pk_context key;
	mbedtls_ssl_config conf;

	(void)arg;
	mbedtls_x509_crt_init(&cert);
	mbedtls_pk_init(&key);
	mbedtls_ssl_config_init(&conf);
	if (mbedtls_x509_crt_parse(&cert, (const unsigned char *)lh_cert,
				   sizeof(lh_cert)) != 0 ||
	    mbedtls_pk_parse_key(&key, (const unsigned char *)lh_key,
				 sizeof(lh_key), NULL, 0) != 0 ||
	    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER,
					MBEDTLS_SSL_TRANSPORT_STREAM,
					MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
	    mbedtls_ssl_conf_own_cert(&conf, &cert, &key) != 0) {
		fprintf(stderr, "fake tenant: setup failed\n");
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
			if (mbedtls_ssl_handshake(&ssl) == 0) {
				serve(&ssl);
				(void)mbedtls_ssl_close_notify(&ssl);
			}
		}
		mbedtls_ssl_free(&ssl);
		(void)close(fd);
	}
	mbedtls_ssl_config_free(&conf);
	mbedtls_pk_free(&key);
	mbedtls_x509_crt_free(&cert);
	return NULL;
}

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
	snprintf(host, sizeof(host), "localhost:%u",
		 (unsigned int)ntohs(server_sa.sin_port));
	return pthread_create(&server_thread, NULL, server, NULL) == 0 ? 0 : -1;
}

static void server_stop(void)
{
	int fd;

	stopping = true;
	(void)shutdown(listen_fd, SHUT_RDWR);
	fd = socket(AF_INET, SOCK_STREAM, 0);
	(void)connect(fd, (struct sockaddr *)&server_sa, sizeof(server_sa));
	(void)close(fd);
	(void)pthread_join(server_thread, NULL);
	(void)close(listen_fd);
}

/* ------------------------------------------------------------------------ */
/* The device side                                                           */
/* ------------------------------------------------------------------------ */

static char reason[160];

/* The result the configuration thread posts, within 15 s. */
static bool wait_event(struct tedge_config_event *ev)
{
	for (int i = 0; i < 1500; i++) {
		if (tedge_config_poll_event(ev) == 0) {
			return true;
		}
		struct timespec ts = { 0, 10 * 1000 * 1000 };

		nanosleep(&ts, NULL);
	}
	fprintf(stderr, "no result from the configuration thread\n");
	failures++;
	return false;
}

static int set_from(const char *file, struct tedge_config_event *ev)
{
	char line[256];
	int ret;

	snprintf(line, sizeof(line), "524,4321,https://%s/roots/%s,trusted-roots",
		 host, file);
	reason[0] = '\0';
	ret = tedge_config_request_download(line, "4321", reason,
					    sizeof(reason));
	if (ret != 0) {
		return ret;
	}
	return wait_event(ev) ? ev->rc : -ETIMEDOUT;
}

static void test_requests(void)
{
	char line[128];
	struct tedge_config_event ev;

	CHECK(tedge_config_types_line(line, sizeof(line)) > 0);
	CHECK(strcmp(line, "119,trusted-roots") == 0);

	/* Refused at once: nothing starts. */
	CHECK(tedge_config_request_upload("526,4321,no-such-type", "1", reason,
					  sizeof(reason)) == -ENOENT);
	CHECK(strcmp(reason, "unknown configuration type no-such-type") == 0);
	CHECK(tedge_config_request_download("524,4321", "1", reason,
					    sizeof(reason)) == -EINVAL);
	CHECK(!tedge_config_busy());

	/* A set: both roots stored, the operation finished by its id. */
	CHECK(set_from("good.pem", &ev) == 0);
	CHECK(strcmp(ev.op_name, "c8y_DownloadConfigFile") == 0);
	CHECK(strcmp(ev.op_id, "4321") == 0);
	CHECK(tedge_trust_count() == 2);

	/* One request at a time. */
	snprintf(line, sizeof(line), "524,4321,https://%s/roots/good.pem,"
		 "trusted-roots", host);
	CHECK(tedge_config_request_download(line, "1", reason,
					    sizeof(reason)) == 0);
	CHECK(tedge_config_request_upload("526,4321,trusted-roots", "2",
					  reason, sizeof(reason)) == -EBUSY);
	CHECK(strncmp(reason, "busy", 4) == 0);
	CHECK(wait_event(&ev) && ev.rc == 0);

	/* A set the type refuses: its reason, and the roots in force stay. */
	CHECK(set_from("leaf.pem", &ev) != 0);
	CHECK(strstr(ev.text, "certificate 1 (CN=download.test): not a CA") !=
	      NULL);
	CHECK(tedge_trust_count() == 2);

	/* A download that fails, and one that is too big. */
	CHECK(set_from("missing.pem", &ev) != 0);
	CHECK(strncmp(ev.text, "download failed", 15) == 0);
	CHECK(set_from("big.pem", &ev) == -EFBIG);
	CHECK(strstr(ev.text, "larger than the 16384 bytes") != NULL);
	CHECK(tedge_trust_count() == 2);

	/* A get: an event of the configuration's type on the device, and the
	 * file as its binary; the operation finishes with the binary's URL. */
	CHECK(tedge_config_request_upload("526,4321,trusted-roots", "5150",
					  reason, sizeof(reason)) == 0);
	CHECK(wait_event(&ev));
	CHECK(ev.rc == 0);
	CHECK(strcmp(ev.op_name, "c8y_UploadConfigFile") == 0);
	CHECK(strcmp(ev.op_id, "5150") == 0);
	{
		char url[96];

		snprintf(url, sizeof(url), "https://%s/event/events/77/binaries",
			 host);
		CHECK(strcmp(ev.text, url) == 0);
	}
	CHECK(strstr(event_body, "\"source\":{\"id\":\"4242\"}") != NULL);
	CHECK(strstr(event_body, "\"type\":\"trusted-roots\"") != NULL);
	CHECK(strstr(binary_headers, "filename=\"trusted-roots-") != NULL);
	CHECK(strncmp(binary_body, "# tiny-edge trusted-roots\n", 26) == 0);
	CHECK(strstr(binary_body, ca1) != NULL);
	CHECK(strstr(binary_body, ca2) != NULL);
}

int main(void)
{
	char dir[] = "/tmp/tedge-config-test-XXXXXX";
	struct tedge_posix_config cfg = { 0 };

	memset(big_file, '#', sizeof(big_file));
	for (size_t i = 63; i < sizeof(big_file); i += 64) {
		big_file[i] = '\n';
	}
	if (mkdtemp(dir) == NULL || psa_crypto_init() != PSA_SUCCESS) {
		return 1;
	}
	cfg.state_dir = dir;
	if (tedge_posix_configure(&cfg) != 0 || tedge_port_kv_init() != 0 ||
	    server_start() != 0) {
		return 1;
	}
	/* The fake tenant's certificate comes from dl_ca. */
	CHECK(tedge_port_tls_set_ca(dl_ca, sizeof(dl_ca)) == 0);
	CHECK(tedge_c8y_host_set(host) == 0);
	CHECK(tedge_trust_register() == 0);

	test_requests();

	server_stop();
	{
		char rm[64];

		snprintf(rm, sizeof(rm), "rm -rf %s", dir);
		(void)!system(rm);
	}
	if (failures == 0) {
		printf("all configuration-management checks passed\n");
	}
	return failures == 0 ? 0 : 1;
}
