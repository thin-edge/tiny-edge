/* SPDX-License-Identifier: Apache-2.0
 *
 * The stored roots (core/src/tedge_trust.c) on the POSIX port's key-value
 * store: what a set accepts and refuses, that a refused or unfinished set
 * leaves the one in force, the file a get produces, and that a stored root
 * reaches the port: a download connection to a server it issued succeeds.
 */

#include "tedge_internal.h"

#include <tedge/port/kv.h>
#include <tedge/posix.h>

#include <psa/crypto.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#include "test_trust_certs.h"

static const char godaddy[] = {
#include "tedge_ca_bundle.inc"
	0x00
};

static const char download_roots[] = {
#include "tedge_download_ca.inc"
	0x00
};

static char reason[160];

/* Set the stored roots to the concatenation of the PEM texts given, fed
 * @p piece bytes at a time. @return what tedge_trust_write_end() said. */
static int set_roots(size_t piece, const char *const pems[], size_t n)
{
	int ret = tedge_trust_write_begin();

	reason[0] = '\0';
	if (ret != 0) {
		return ret;
	}
	for (size_t i = 0; i < n; i++) {
		const char *p = pems[i];
		size_t left = strlen(p);

		while (left > 0) {
			size_t take = (left < piece) ? left : piece;

			(void)tedge_trust_write_chunk(p, take);
			p += take;
			left -= take;
		}
	}
	return tedge_trust_write_end(reason, sizeof(reason));
}

#define SET(...)                                                               \
	set_roots(4096, (const char *const[]){ __VA_ARGS__ },                  \
		  sizeof((const char *const[]){ __VA_ARGS__ }) / sizeof(char *))

/* The get file, collected. */
static char file[8192];
static size_t file_len;

static int collect(const void *data, size_t len, void *ctx)
{
	(void)ctx;
	if (file_len + len >= sizeof(file)) {
		return -ENOSPC;
	}
	memcpy(file + file_len, data, len);
	file_len += len;
	file[file_len] = '\0';
	return 0;
}

static const char *read_file(void)
{
	file_len = 0;
	file[0] = '\0';
	CHECK(tedge_trust_read(collect, NULL) == 0);
	return file;
}

static char active(void)
{
	char bank = 0;

	(void)tedge_port_kv_get("tedge/trust/download/active", &bank, 1);
	return bank;
}

static void test_accepts(void)
{
	char copy[sizeof(file)];

	CHECK(tedge_trust_register() == 0);
	CHECK(tedge_trust_count() == 0);
	CHECK(try_connect(TEDGE_PORT_TLS_DOWNLOAD) == -EACCES);

	/* A root that issued the server: downloads trust it at once, the
	 * tenant's connections never do. */
	CHECK(SET(dl_ca) == 0);
	CHECK(tedge_trust_count() == 1);
	CHECK(try_connect(TEDGE_PORT_TLS_DOWNLOAD) == 0);
	CHECK(try_connect(TEDGE_PORT_TLS) == -EACCES);

	/* A whole new set replaces it. */
	CHECK(SET(ca1, ca2) == 0);
	CHECK(tedge_trust_count() == 2);
	CHECK(try_connect(TEDGE_PORT_TLS_DOWNLOAD) == -EACCES);

	/* The file names the compiled-in roots and holds the stored ones. */
	read_file();
	CHECK(strncmp(file, "# tiny-edge trusted-roots\n", 26) == 0);
	CHECK(strstr(file, "# built-in (read-only): CN=Go Daddy Root "
			   "Certificate Authority - G2\n") != NULL);
	CHECK(strstr(file, "# built-in (read-only): CN=ISRG Root X1\n") != NULL);
	CHECK(strstr(file, "# built-in (read-only): CN=USERTrust ECC "
			   "Certification Authority\n") != NULL);
	CHECK(strstr(file, "# 1: CN=tiny-edge test CA 1, expires 2126-") !=
	      NULL);
	CHECK(strstr(file, "# 2: CN=tiny-edge test CA 2, expires 2126-") !=
	      NULL);
	/* openssl writes the same 64-column PEM. */
	CHECK(strstr(file, ca1) != NULL);
	CHECK(strstr(file, ca2) != NULL);

	/* The file goes back in unchanged: a get, edit, set round trip. */
	memcpy(copy, file, file_len + 1);
	CHECK(SET(copy) == 0);
	CHECK(tedge_trust_count() == 2);
	CHECK(strcmp(read_file(), copy) == 0);

	/* Copies of compiled-in roots and repeats are skipped, CRLF line
	 * endings read, and the file can come a byte at a time. */
	{
		static char crlf[4096];
		size_t o = 0;

		for (const char *p = ca3; *p != '\0'; p++) {
			if (*p == '\n') {
				crlf[o++] = '\r';
			}
			crlf[o++] = *p;
		}
		CHECK(set_roots(1, (const char *const[]){ "intro text\n",
				godaddy, download_roots, crlf, ca3 }, 5) == 0);
		CHECK(tedge_trust_count() == 1);
		read_file();
		CHECK(strstr(file, "# 1: CN=tiny-edge test CA 3,") != NULL);
		CHECK(strstr(file, "# 2:") == NULL);
	}

	/* An empty file clears the stored roots. */
	CHECK(SET("") == 0);
	CHECK(tedge_trust_count() == 0);
	CHECK(strstr(read_file(), "BEGIN") == NULL);
}

/* Each refusal names the certificate and leaves the set in force. */
static void refused(const char *pem_a, const char *pem_b, const char *why)
{
	char before[sizeof(file)];
	char bank = active();
	int ret;

	memcpy(before, read_file(), file_len + 1);
	ret = (pem_b != NULL) ? SET(pem_a, pem_b) : SET(pem_a);
	CHECK(ret != 0);
	if (strstr(reason, why) == NULL) {
		fprintf(stderr, "  reason \"%s\" lacks \"%s\"\n", reason, why);
		failures++;
	}
	CHECK(active() == bank);
	CHECK(strcmp(read_file(), before) == 0);
}

static void test_refuses(void)
{
	CHECK(SET(ca1) == 0);
	refused(srv_cert, NULL, "certificate 1 (CN=download.test): not a CA");
	refused(ca2, expired_ca, "certificate 2 (CN=tiny-edge test expired "
				 "CA): expired on 2021-01-01");
	refused(future_ca, NULL, "not valid until 2100-01-01");
	refused(nosign_ca, NULL, "key usage does not allow signing");
	refused(big_ca, NULL, "1536");
	{
		const char *five[] = { ca1, ca2, ca3, ca4, ca5 };

		CHECK(set_roots(4096, five, 5) != 0);
		CHECK(strstr(reason, "more than 4 certificates") != NULL);
	}
	refused("-----BEGIN CERTIFICATE-----\nnot base64 at all\n"
		"-----END CERTIFICATE-----\n", NULL, "certificate 1: cannot be "
		"parsed");
	refused("-----BEGIN CERTIFICATE-----\nMIIB\n", NULL,
		"certificate 1: no \"-----END CERTIFICATE-----\"");
	CHECK(tedge_trust_count() == 1);
}

static void test_unfinished(void)
{
	char bank;
	char key[48];
	uint8_t der[2048];

	CHECK(SET(ca1) == 0);
	bank = active();

	/* One write at a time. */
	CHECK(tedge_trust_write_begin() == 0);
	CHECK(tedge_trust_write_begin() == -EBUSY);

	/* A set that never ends, as after a reset: the new certificate is in
	 * the other bank, and the one in force is untouched. */
	CHECK(tedge_trust_write_chunk(ca2, strlen(ca2)) == 0);
	tedge_trust_write_abort();
	CHECK(active() == bank);
	snprintf(key, sizeof(key), "tedge/trust/download/%c/0",
		 (bank == 'a') ? 'b' : 'a');
	CHECK(tedge_port_kv_get(key, der, sizeof(der)) > 0);
	CHECK(tedge_trust_count() == 1);
	CHECK(strstr(read_file(), "CN=tiny-edge test CA 1,") != NULL);

	/* The next set overwrites that bank, and a shorter set leaves no
	 * certificate of a longer one behind. */
	CHECK(SET(ca3, ca4, ca5) == 0);
	CHECK(SET(ca2) == 0);
	snprintf(key, sizeof(key), "tedge/trust/download/%c/1", active());
	CHECK(tedge_port_kv_get(key, der, sizeof(der)) == -ENOENT);
	CHECK(tedge_trust_count() == 1);
}

int main(void)
{
	char dir[] = "/tmp/tedge-trust-test-XXXXXX";
	struct tedge_posix_config cfg = { 0 };

	if (mkdtemp(dir) == NULL || psa_crypto_init() != PSA_SUCCESS) {
		return 1;
	}
	cfg.state_dir = dir;
	if (tedge_posix_configure(&cfg) != 0 || tedge_port_kv_init() != 0 ||
	    server_start() != 0) {
		return 1;
	}
	CHECK(tedge_port_tls_set_ca(godaddy, sizeof(godaddy)) == 0);

	test_accepts();
	test_refuses();
	test_unfinished();

	server_stop();
	{
		char rm[64];

		snprintf(rm, sizeof(rm), "rm -rf %s", dir);
		(void)!system(rm);
	}
	if (failures == 0) {
		printf("all trusted-roots checks passed\n");
	}
	return failures == 0 ? 0 : 1;
}
