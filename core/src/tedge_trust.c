/* SPDX-License-Identifier: Apache-2.0
 *
 * The roots the client trusts beyond the tenant's own (TEDGE_C8Y_CA_FILES).
 *
 * Downloads from hosts outside the tenant (tedge_http_security()) are also
 * verified against:
 *
 *   - the roots compiled in from TEDGE_DOWNLOAD_CA_FILES, one PEM bundle;
 *   - with TEDGE_TRUSTED_ROOTS, the roots an operator stored, one DER
 *     certificate each.
 *
 * Nothing here can remove a compiled-in root: a stored set only adds.
 *
 * Storage (tedge/port/kv.h), one bank in use and one being written:
 *
 *   tedge/trust/download/active   "a" or "b"
 *   tedge/trust/download/a/<n>    DER, n = 0, 1, ... up to the first gap
 *   tedge/trust/download/b/<n>
 *
 * A new set is written to the bank not in use, each certificate as soon as
 * it has been checked, and takes over with the single write of "active".
 * A reset before that write leaves the old set in force. Writing as the
 * file arrives means the file is never held whole: the most a set needs is
 * one PEM block.
 */

#include "tedge_internal.h"
#include "tedge_util.h"

#include <tedge/port/kv.h>
#include <tedge/port/log.h>
#include <tedge/port/net.h>
#include <tedge/port/os.h>
#include <tedge/port/time.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(TEDGE_TRUSTED_ROOTS)
#include <mbedtls/x509_crt.h>
#endif

TEDGE_LOG_DECLARE();

/* The roots listed in TEDGE_DOWNLOAD_CA_FILES. Just the NUL when the list
 * is empty. */
static const unsigned char download_ca[] = {
#include "tedge_download_ca.inc"
	0x00
};

#if !defined(TEDGE_TRUSTED_ROOTS)

int tedge_trust_register(void)
{
	static const struct tedge_port_ca_buf bufs[] = {
		{ download_ca, sizeof(download_ca) },
	};

	if (sizeof(download_ca) == 1) {
		return 0;
	}
	return tedge_port_tls_set_extra_ca(TEDGE_PORT_CA_DOWNLOAD, bufs, 1);
}

#else /* TEDGE_TRUSTED_ROOTS */

#define MAX_CERTS  TEDGE_TRUSTED_ROOTS_MAX
#define MAX_DER    TEDGE_TRUSTED_ROOTS_MAX_DER
#define KEY_PREFIX "tedge/trust/download"
/* One PEM block: base64 of MAX_DER in 64-character lines, the armour, and
 * a line ending of up to two bytes each. */
#define PEM_LINES  ((MAX_DER * 4 / 3 + 63) / 64)
#define PEM_CAP    ((MAX_DER * 4 / 3 + 3) + PEM_LINES * 2 + 64 + 1)
#define LINE_CAP   80
#define BEGIN_LINE "-----BEGIN CERTIFICATE-----"
#define END_LINE   "-----END CERTIFICATE-----"

/* ------------------------------------------------------------------------ */
/* The set in force                                                          */
/* ------------------------------------------------------------------------ */

/* The stored certificates, back to back in one block of the client's heap,
 * and the buffer list the port holds. The port keeps the pointers until the
 * next call, so a new set gets the other list and the old block is freed
 * only after the port has let go of it. */
static struct {
	uint8_t *block;
	size_t count;
	size_t offset[MAX_CERTS];
	size_t len[MAX_CERTS];
	char bank; /* 'a' or 'b', the bank in force; 0 before loading */
} loaded;
static struct tedge_port_ca_buf port_bufs[2][1 + MAX_CERTS];
static int port_list;
static bool registered;
TEDGE_PORT_MUTEX_DEFINE(trust_lock);

static void key_of(char *key, size_t cap, char bank, size_t n)
{
	(void)snprintf(key, cap, KEY_PREFIX "/%c/%u", bank, (unsigned int)n);
}

static char active_bank(void)
{
	char bank = 0;

	if (tedge_port_kv_get(KEY_PREFIX "/active", &bank, 1) == 1 &&
	    (bank == 'a' || bank == 'b')) {
		return bank;
	}
	return 'a'; /* nothing stored yet: an empty bank */
}

/* Read bank @p bank into a new block. */
static int load_bank(char bank, uint8_t **block, size_t *count,
		     size_t offset[], size_t len[])
{
	char key[48];
	uint8_t *scratch = tedge_alloc(MAX_DER);
	size_t total = 0, n = 0;
	int ret = 0;

	*block = NULL;
	*count = 0;
	if (scratch == NULL) {
		return -ENOMEM;
	}
	/* First the lengths, so the block is allocated once. */
	for (; n < MAX_CERTS; n++) {
		key_of(key, sizeof(key), bank, n);
		ret = tedge_port_kv_get(key, scratch, MAX_DER);
		if (ret <= 0) {
			break;
		}
		offset[n] = total;
		len[n] = (size_t)ret;
		total += (size_t)ret;
	}
	ret = (ret == -ENOENT || ret >= 0) ? 0 : ret;
	if (ret == 0 && n > 0) {
		*block = tedge_alloc(total);
		if (*block == NULL) {
			ret = -ENOMEM;
		}
	}
	for (size_t i = 0; ret == 0 && i < n; i++) {
		key_of(key, sizeof(key), bank, i);
		if (tedge_port_kv_get(key, *block + offset[i], len[i]) !=
		    (int)len[i]) {
			ret = -EIO;
		}
	}
	tedge_free(scratch);
	if (ret != 0) {
		tedge_free(*block);
		*block = NULL;
		return ret;
	}
	*count = n;
	return 0;
}

/* Hand the compiled-in bundle and the set in force to the port. Called
 * with trust_lock held. */
static int give_to_port(void)
{
	struct tedge_port_ca_buf *bufs = port_bufs[port_list];
	size_t n = 0;
	int ret;

	if (sizeof(download_ca) > 1) {
		bufs[n++] = (struct tedge_port_ca_buf){ download_ca,
							sizeof(download_ca) };
	}
	for (size_t i = 0; i < loaded.count; i++) {
		bufs[n++] = (struct tedge_port_ca_buf){
			loaded.block + loaded.offset[i], loaded.len[i]
		};
	}
	ret = tedge_port_tls_set_extra_ca(TEDGE_PORT_CA_DOWNLOAD, bufs, n);
	port_list ^= 1;
	return ret;
}

/* Load the bank in force and give it to the port, replacing what it had. */
static int apply(void)
{
	uint8_t *block, *old;
	size_t count, offset[MAX_CERTS] = { 0 }, len[MAX_CERTS] = { 0 };
	char bank = active_bank();
	int ret = load_bank(bank, &block, &count, offset, len);

	if (ret != 0) {
		TEDGE_LOG_ERR("trusted roots: cannot read the stored set (%d); "
			      "using the compiled-in roots only", ret);
		count = 0;
		block = NULL;
	}
	tedge_port_mutex_lock(&trust_lock);
	old = loaded.block;
	loaded.block = block;
	loaded.count = count;
	memcpy(loaded.offset, offset, sizeof(offset));
	memcpy(loaded.len, len, sizeof(len));
	loaded.bank = bank;
	ret = give_to_port();
	registered = true;
	tedge_port_mutex_unlock(&trust_lock);
	tedge_free(old);
	if (ret != 0) {
		TEDGE_LOG_WRN("trusted roots: the port did not take them (%d)",
			      ret);
	}
	TEDGE_LOG_INF("trusted roots: %u stored for downloads",
		      (unsigned int)count);
	return ret;
}

int tedge_trust_register(void)
{
	bool done;

	tedge_port_mutex_lock(&trust_lock);
	done = registered;
	tedge_port_mutex_unlock(&trust_lock);
	return done ? 0 : apply();
}

size_t tedge_trust_count(void)
{
	size_t n;

	tedge_port_mutex_lock(&trust_lock);
	n = loaded.count;
	tedge_port_mutex_unlock(&trust_lock);
	return n;
}

/* ------------------------------------------------------------------------ */
/* Describing a certificate                                                  */
/* ------------------------------------------------------------------------ */

static time_t to_epoch(const mbedtls_x509_time *t)
{
	struct tm tm = {
		.tm_year = t->year - 1900,
		.tm_mon = t->mon - 1,
		.tm_mday = t->day,
		.tm_hour = t->hour,
		.tm_min = t->min,
		.tm_sec = t->sec,
	};

	return tedge_timegm(&tm);
}

/* The subject's CN, or the whole subject when it has none. */
static void subject_of(const mbedtls_x509_crt *crt, char *out, size_t cap)
{
	char dn[160];
	const char *cn;

	if (mbedtls_x509_dn_gets(dn, sizeof(dn), &crt->subject) < 0) {
		(void)snprintf(out, cap, "?");
		return;
	}
	cn = strstr(dn, "CN=");
	(void)snprintf(out, cap, "%s", (cn != NULL) ? cn : dn);
	/* A CN is the last component in practice; cut anything after it. */
	if (cn != NULL) {
		char *comma = strstr(out, ", ");

		if (comma != NULL) {
			*comma = '\0';
		}
	}
}

static void date_of(const mbedtls_x509_time *t, char *out, size_t cap)
{
	(void)snprintf(out, cap, "%04d-%02d-%02d", t->year, t->mon, t->day);
}

/* ------------------------------------------------------------------------ */
/* Writing a new set                                                         */
/* ------------------------------------------------------------------------ */

static struct {
	bool open;
	char bank;            /* the bank being written */
	size_t position;      /* certificates seen, duplicates included */
	size_t stored;        /* certificates written */
	uint32_t seen_hash[MAX_CERTS];
	size_t seen_len[MAX_CERTS];
	mbedtls_x509_crt builtin;
	bool builtin_parsed;
	char line[LINE_CAP];
	size_t line_len;
	bool line_overflow;
	char *pem;            /* the block being collected; NULL outside one */
	size_t pem_len;
	int error;            /* the first failure; 0 so far */
	char reason[128];
} w;
TEDGE_PORT_MUTEX_DEFINE(write_lock);

static int fail(int err, const char *fmt, ...)
{
	va_list ap;

	if (w.error == 0) {
		w.error = err;
		va_start(ap, fmt);
		(void)vsnprintf(w.reason, sizeof(w.reason), fmt, ap);
		va_end(ap);
	}
	return w.error;
}

static uint32_t fnv1a(const uint8_t *p, size_t len)
{
	uint32_t h = 2166136261u;

	for (size_t i = 0; i < len; i++) {
		h = (h ^ p[i]) * 16777619u;
	}
	return h;
}

static bool is_builtin(const mbedtls_x509_crt *crt)
{
	for (const mbedtls_x509_crt *b = &w.builtin; w.builtin_parsed &&
	     b != NULL && b->raw.len > 0; b = b->next) {
		if (b->raw.len == crt->raw.len &&
		    memcmp(b->raw.p, crt->raw.p, crt->raw.len) == 0) {
			return true;
		}
	}
	return false;
}

static bool seen_before(const mbedtls_x509_crt *crt, uint32_t hash)
{
	for (size_t i = 0; i < w.stored; i++) {
		if (w.seen_hash[i] == hash && w.seen_len[i] == crt->raw.len) {
			return true;
		}
	}
	return false;
}

/* One whole PEM block has arrived: check it, and store it. */
static void take_block(void)
{
	mbedtls_x509_crt crt;
	char subject[64], date[16], key[48];
	time_t now;
	uint32_t hash;
	size_t n = ++w.position;
	int ret;

	w.pem[w.pem_len] = '\0';
	mbedtls_x509_crt_init(&crt);
	ret = mbedtls_x509_crt_parse(&crt, (const unsigned char *)w.pem,
				     w.pem_len + 1);
	if (ret != 0) {
		(void)fail(-EINVAL, "certificate %u: cannot be parsed, or uses "
			   "a key this device does not support (-0x%04x)",
			   (unsigned int)n, (unsigned int)((ret < 0) ? -ret : ret));
		goto out;
	}
	subject_of(&crt, subject, sizeof(subject));
	if (crt.raw.len > MAX_DER) {
		(void)fail(-EFBIG, "certificate %u (%s): %u bytes, more than "
			   "the %u this device accepts", (unsigned int)n,
			   subject, (unsigned int)crt.raw.len,
			   (unsigned int)MAX_DER);
		goto out;
	}
	hash = fnv1a(crt.raw.p, crt.raw.len);
	if (is_builtin(&crt) || seen_before(&crt, hash)) {
		goto out; /* trusted already: nothing to store */
	}
	if (mbedtls_x509_crt_get_ca_istrue(&crt) != 1) {
		(void)fail(-EINVAL, "certificate %u (%s): not a CA certificate",
			   (unsigned int)n, subject);
		goto out;
	}
	if (mbedtls_x509_crt_check_key_usage(&crt,
					     MBEDTLS_X509_KU_KEY_CERT_SIGN) != 0) {
		(void)fail(-EINVAL, "certificate %u (%s): its key usage does "
			   "not allow signing certificates", (unsigned int)n,
			   subject);
		goto out;
	}
	if (tedge_port_time_now(&now) == 0) {
		if (now > to_epoch(&crt.valid_to)) {
			date_of(&crt.valid_to, date, sizeof(date));
			(void)fail(-EINVAL, "certificate %u (%s): expired on %s",
				   (unsigned int)n, subject, date);
			goto out;
		}
		if (now < to_epoch(&crt.valid_from)) {
			date_of(&crt.valid_from, date, sizeof(date));
			(void)fail(-EINVAL, "certificate %u (%s): not valid "
				   "until %s", (unsigned int)n, subject, date);
			goto out;
		}
	}
	if (w.stored >= MAX_CERTS) {
		(void)fail(-ENOSPC, "more than %u certificates: this device "
			   "stores at most %u", (unsigned int)MAX_CERTS,
			   (unsigned int)MAX_CERTS);
		goto out;
	}
	key_of(key, sizeof(key), w.bank, w.stored);
	ret = tedge_port_kv_set(key, crt.raw.p, crt.raw.len);
	if (ret != 0) {
		(void)fail(ret, "certificate %u (%s): storing it failed (%d)",
			   (unsigned int)n, subject, ret);
		goto out;
	}
	w.seen_hash[w.stored] = hash;
	w.seen_len[w.stored] = crt.raw.len;
	w.stored++;
out:
	mbedtls_x509_crt_free(&crt);
}

/* Lines are compared without surrounding white space, so a file with
 * CRLF endings or indented armour still reads. */
static void trim(char *s)
{
	size_t len = strlen(s);
	size_t start = 0;

	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
			   s[len - 1] == '\r')) {
		s[--len] = '\0';
	}
	while (s[start] == ' ' || s[start] == '\t') {
		start++;
	}
	memmove(s, s + start, len - start + 1);
}

static void pem_append(const char *line)
{
	size_t len = strlen(line);

	if (w.pem_len + len + 2 > PEM_CAP) {
		(void)fail(-EFBIG, "certificate %u: more than the %u bytes "
			   "this device accepts", (unsigned int)(w.position + 1),
			   (unsigned int)MAX_DER);
		return;
	}
	memcpy(w.pem + w.pem_len, line, len);
	w.pem_len += len;
	w.pem[w.pem_len++] = '\n';
}

static void take_line(void)
{
	char *line = w.line;

	w.line[w.line_len] = '\0';
	trim(line);
	if (w.line_overflow) {
		/* A long line outside a block is comment text; inside one it
		 * cannot be base64 from any tool. */
		if (w.pem_len > 0) {
			(void)fail(-EINVAL, "certificate %u: a line is too long",
				   (unsigned int)(w.position + 1));
		}
	} else if (w.pem_len == 0) {
		if (strcmp(line, BEGIN_LINE) == 0) {
			pem_append(line);
		}
	} else {
		pem_append(line);
		if (w.error == 0 && strcmp(line, END_LINE) == 0) {
			take_block();
			w.pem_len = 0;
		}
	}
	w.line_len = 0;
	w.line_overflow = false;
}

int tedge_trust_write_begin(void)
{
	size_t len = 0;

	tedge_port_mutex_lock(&write_lock);
	if (w.open) {
		tedge_port_mutex_unlock(&write_lock);
		return -EBUSY;
	}
	memset(&w, 0, sizeof(w));
	w.pem = tedge_alloc(PEM_CAP);
	if (w.pem == NULL) {
		tedge_port_mutex_unlock(&write_lock);
		return -ENOMEM;
	}
	w.open = true;
	w.bank = (active_bank() == 'a') ? 'b' : 'a';
	/* The compiled-in roots, to skip copies of them. */
	mbedtls_x509_crt_init(&w.builtin);
	{
		const unsigned char *tenant = tedge_c8y_server_ca(&len);

		w.builtin_parsed =
			mbedtls_x509_crt_parse(&w.builtin, tenant, len) >= 0;
	}
	if (sizeof(download_ca) > 1) {
		(void)mbedtls_x509_crt_parse(&w.builtin, download_ca,
					     sizeof(download_ca));
	}
	tedge_port_mutex_unlock(&write_lock);
	return 0;
}

int tedge_trust_write_chunk(const void *data, size_t len)
{
	const char *p = data;

	for (size_t i = 0; i < len && w.error == 0; i++) {
		if (p[i] == '\n') {
			take_line();
		} else if (w.line_len < LINE_CAP - 1) {
			w.line[w.line_len++] = p[i];
		} else {
			w.line_overflow = true;
		}
	}
	return w.error;
}

static void write_close(void)
{
	tedge_free(w.pem);
	w.pem = NULL;
	mbedtls_x509_crt_free(&w.builtin);
	w.open = false;
}

int tedge_trust_write_end(char *reason, size_t cap)
{
	char key[48];
	int ret;

	if (w.line_len > 0 && w.error == 0) {
		take_line(); /* the last line had no newline */
	}
	if (w.error == 0 && w.pem_len > 0) {
		(void)fail(-EINVAL, "certificate %u: no \"" END_LINE "\"",
			   (unsigned int)(w.position + 1));
	}
	if (w.error == 0) {
		/* The loader stops at the first gap: remove what an earlier,
		 * longer set left behind. */
		for (size_t i = w.stored; i < MAX_CERTS; i++) {
			key_of(key, sizeof(key), w.bank, i);
			(void)tedge_port_kv_delete(key);
		}
		ret = tedge_port_kv_set(KEY_PREFIX "/active", &w.bank, 1);
		if (ret != 0) {
			(void)fail(ret, "storing the new set failed (%d)", ret);
		}
	}
	ret = w.error;
	if (ret != 0 && reason != NULL && cap > 0) {
		(void)snprintf(reason, cap, "%s", w.reason);
	}
	tedge_port_mutex_lock(&write_lock);
	write_close();
	tedge_port_mutex_unlock(&write_lock);
	if (ret != 0) {
		TEDGE_LOG_WRN("trusted roots: new set refused: %s", w.reason);
		return ret;
	}
	TEDGE_LOG_INF("trusted roots: new set of %u stored",
		      (unsigned int)w.stored);
	(void)apply();
	return 0;
}

void tedge_trust_write_abort(void)
{
	tedge_port_mutex_lock(&write_lock);
	if (w.open) {
		write_close();
	}
	tedge_port_mutex_unlock(&write_lock);
}

/* ------------------------------------------------------------------------ */
/* Reading the set                                                           */
/* ------------------------------------------------------------------------ */

static int put(tedge_sink_fn sink, void *ctx, const char *s)
{
	return sink(s, strlen(s), ctx);
}

/* "# built-in (read-only): CN=..." for each root in @p pem. */
static int list_builtin(tedge_sink_fn sink, void *ctx,
			const unsigned char *pem, size_t len)
{
	mbedtls_x509_crt chain;
	char subject[64], line[96];
	int ret = 0;

	mbedtls_x509_crt_init(&chain);
	if (mbedtls_x509_crt_parse(&chain, pem, len) >= 0) {
		for (const mbedtls_x509_crt *c = &chain;
		     ret == 0 && c != NULL && c->raw.len > 0; c = c->next) {
			subject_of(c, subject, sizeof(subject));
			(void)snprintf(line, sizeof(line),
				       "# built-in (read-only): %s\n", subject);
			ret = put(sink, ctx, line);
		}
	}
	mbedtls_x509_crt_free(&chain);
	return ret;
}

static int write_pem(tedge_sink_fn sink, void *ctx, const uint8_t *der,
		     size_t len)
{
	char b64[65];
	int ret = put(sink, ctx, BEGIN_LINE "\n");

	for (size_t i = 0; ret == 0 && i < len; i += 48) {
		(void)tedge_base64_encode(der + i, TEDGE_MIN(48, len - i), b64,
					  sizeof(b64));
		ret = put(sink, ctx, b64);
		if (ret == 0) {
			ret = put(sink, ctx, "\n");
		}
	}
	return (ret == 0) ? put(sink, ctx, END_LINE "\n") : ret;
}

int tedge_trust_read(tedge_sink_fn sink, void *ctx)
{
	const unsigned char *tenant;
	size_t len = 0;
	time_t now;
	bool have_time = tedge_port_time_now(&now) == 0;
	int ret;

	ret = put(sink, ctx, "# tiny-edge trusted-roots\n");
	tenant = tedge_c8y_server_ca(&len);
	if (ret == 0) {
		ret = list_builtin(sink, ctx, tenant, len);
	}
	if (ret == 0 && sizeof(download_ca) > 1) {
		ret = list_builtin(sink, ctx, download_ca, sizeof(download_ca));
	}
	tedge_port_mutex_lock(&trust_lock);
	for (size_t i = 0; ret == 0 && i < loaded.count; i++) {
		const uint8_t *der = loaded.block + loaded.offset[i];
		mbedtls_x509_crt crt;
		char subject[64] = "?", date[16] = "?", line[128];
		bool expired = false;

		mbedtls_x509_crt_init(&crt);
		if (mbedtls_x509_crt_parse_der(&crt, der, loaded.len[i]) == 0) {
			subject_of(&crt, subject, sizeof(subject));
			date_of(&crt.valid_to, date, sizeof(date));
			expired = have_time && now > to_epoch(&crt.valid_to);
		}
		mbedtls_x509_crt_free(&crt);
		(void)snprintf(line, sizeof(line), "# %u: %s, expires %s%s\n",
			       (unsigned int)(i + 1), subject, date,
			       expired ? " (EXPIRED)" : "");
		ret = put(sink, ctx, line);
		if (ret == 0) {
			ret = write_pem(sink, ctx, der, loaded.len[i]);
		}
	}
	tedge_port_mutex_unlock(&trust_lock);
	return ret;
}

#endif /* TEDGE_TRUSTED_ROOTS */
