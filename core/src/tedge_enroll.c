/* SPDX-License-Identifier: Apache-2.0
 *
 * Onboarding with the Cumulocity CA: the device makes its own key and
 * one-time password, shows a registration URL, and enrolls over EST
 * (simpleenroll) as soon as an operator registers the external ID.
 *
 *   key (PSA ITS) -> one-time password -> registration URL
 *     -> CSR signed inside PSA -> POST /.well-known/est/simpleenroll
 *        with Basic <external id>:<one-time password>
 *     -> PKCS#7 reply -> certificate -> TLS credentials
 *
 * The key belongs to the port (tedge/port/crypto.h): it signs the CSR there,
 * and the port pairs it with the certificate for TLS.
 */

#include "tedge_http.h"
#include "tedge_internal.h"

#include "tedge_util.h"

#include <tedge/port/crypto.h>
#include <tedge/port/kv.h>
#include <tedge/port/log.h>
#include <tedge/port/net.h>
#include <tedge/port/os.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

TEDGE_LOG_DECLARE();

#define OTP_LEN 32 /* passwords the device generates */
/* Passwords supplied by the application (tedge_set_enroll_otp), e.g. minted
 * by a provisioning server, may be any length up to this. */
#define OTP_MAX TEDGE_OTP_MAX

#define BASE64_LEN(n) ((((n) + 2) / 3) * 4)
/* One call to tedge_auth_prepare() polls for about this long before it hands
 * control back to the core (which checks the network and calls again). */
#define ENROLL_WINDOW_MS (2 * 60 * 1000)
#define EST_PATH "/.well-known/est/simpleenroll"

/* Kept while the credential is registered. */
static uint8_t cert_der[1024];
static size_t cert_der_len;

static char otp[OTP_MAX + 1];
static bool credentials_ready;

/* Transient buffers from the module heap, only while enrolling. */
struct enroll_bufs {
	char csr_body[1024];
	char resp[4096];
	uint8_t http_rx[1024];
	uint8_t der[1536];
};
static struct enroll_bufs *bufs;
static size_t resp_len;
static uint16_t resp_status;

/* ------------------------------------------------------------------------ */
/* Settings                                                                  */
/* ------------------------------------------------------------------------ */

/* The length of the value stored under @p key, 0 when there is none. */
static size_t stored(const char *key, void *buf, size_t cap)
{
	int n = tedge_port_kv_get(key, buf, cap);

	return (n > 0) ? (size_t)n : 0;
}

/* ------------------------------------------------------------------------ */
/* Key and one-time password                                                 */
/* ------------------------------------------------------------------------ */

static void ensure_otp(void)
{
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	uint8_t rnd[OTP_LEN];

	if (otp[0] != '\0') {
		return;
	}
	/* A stored password wins, whatever its length: it may have been
	 * issued outside the device and already registered in Cumulocity. */
	size_t n = stored(TEDGE_KEY_ENROLL_OTP, otp, OTP_MAX);

	if (n > 0 && n <= OTP_MAX) {
		otp[n] = '\0';
		return;
	}
	(void)tedge_port_random(rnd, sizeof(rnd));
	for (size_t i = 0; i < OTP_LEN; i++) {
		otp[i] = alphabet[rnd[i] % (sizeof(alphabet) - 1)];
	}
	otp[OTP_LEN] = '\0';
	(void)tedge_port_kv_set(TEDGE_KEY_ENROLL_OTP, otp, OTP_LEN);
}

int tedge_set_enroll_otp(const char *password)
{
	size_t n;

	if (!tedge_otp_valid(password)) {
		return -EINVAL;
	}
	n = strlen(password);
	memcpy(otp, password, n);
	otp[n] = '\0';

	/* A supplied password means the device is being onboarded (again),
	 * typically for another tenant or external ID. A certificate from an
	 * earlier enrollment belongs to that old registration: kept, it would
	 * be presented instead of enrolling, and the new tenant would refuse
	 * it. The key pair stays; only the certificate is discarded. */
	if (cert_der_len > 0 || credentials_ready) {
		TEDGE_LOG_WRN("discarding the certificate of the previous enrollment");
	}
	(void)tedge_port_kv_delete(TEDGE_KEY_ENROLL_CERT);
	cert_der_len = 0;
	credentials_ready = false;

	(void)tedge_port_kv_set(TEDGE_KEY_ENROLL_OTP_EXT, "1", 1);
	return tedge_port_kv_set(TEDGE_KEY_ENROLL_OTP, otp, n);
}

/* Whether the stored password was supplied from outside. Persisted, so a
 * reboot before enrollment does not turn it back into a showable one. */
static bool otp_supplied(void)
{
	char flag;

	return stored(TEDGE_KEY_ENROLL_OTP_EXT, &flag, sizeof(flag)) > 0;
}

int tedge_registration_url(char *buf, size_t len)
{
	int n;

	if (buf == NULL || len == 0) {
		return -EINVAL;
	}
	if (credentials_ready || cert_der_len > 0) {
		return -ENOENT; /* already enrolled */
	}
	if (otp_supplied()) {
		/* Whoever issued it already registered the device, and it may
		 * have reached the device sealed end to end: showing it would
		 * undo that. There is nothing for an operator to do. */
		return -EACCES;
	}
	ensure_otp();
	n = snprintf(buf, len,
		     "https://%s/apps/devicemanagement/index.html#/"
		     "deviceregistration?externalId=%s&one-time-password=%s",
		     tedge_c8y_host(), tedge_identity()->external_id, otp);
	return (n > 0 && (size_t)n < len) ? n : -ENOSPC;
}

/* The URL carries the one-time password, so it is only logged at debug
 * level; the application shows it through tedge_registration_url(). */
static void log_registration_hint(void)
{
	char url[220];

	if (tedge_registration_url(url, sizeof(url)) > 0) {
		TEDGE_LOG_INF("waiting to be registered as \"%s\" on %s",
			tedge_identity()->external_id, tedge_c8y_host());
		TEDGE_LOG_DBG("registration URL: %s", url);
	}
}

/* ------------------------------------------------------------------------ */
/* CSR                                                                       */
/* ------------------------------------------------------------------------ */

static int make_csr_internal(void);

/* Shared with renewal: build a CSR for the device key into the shared
 * buffer, and post it to an EST endpoint. */
int tedge_est_make_csr(char *out, size_t len)
{
	int ret;

	if (bufs == NULL) {
		bufs = tedge_alloc(sizeof(*bufs));
		if (bufs == NULL) {
			return -ENOMEM;
		}
	}
	ret = make_csr_internal();
	if (ret == 0 && out != NULL) {
		snprintf(out, len, "%s", bufs->csr_body);
	}
	return ret;
}

static int make_csr_internal(void)
{
	char subject[80];
	char *pem = bufs->resp; /* reused as scratch */
	const size_t pem_cap = 1200;
	int ret;

	snprintf(subject, sizeof(subject), "CN=%s", tedge_identity()->external_id);
	ret = tedge_port_device_csr(subject, pem, pem_cap);
	if (ret < 0) {
		return ret;
	}
	/* EST wants the base64 body without the PEM armour. */
	{
		const char *b = strstr(pem, "-----\n");
		const char *e = strstr(pem, "-----END");

		if (b == NULL || e == NULL) {
			return -EINVAL;
		}
		b += 6;
		snprintf(bufs->csr_body, sizeof(bufs->csr_body), "%.*s",
			 (int)(e - b), b);
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* HTTPS POST to the tenant                                                  */
/* ------------------------------------------------------------------------ */

static int on_body(int status, const void *data, size_t length, void *ctx)
{
	size_t n = TEDGE_MIN(length, sizeof(bufs->resp) - 1 - resp_len);

	TEDGE_UNUSED(status);
	TEDGE_UNUSED(ctx);
	memcpy(bufs->resp + resp_len, data, n);
	resp_len += n;
	bufs->resp[resp_len] = '\0';
	return 0;
}

static int https_post(const char *path, const char *auth, const char *body)
{
	const char *headers[] = { auth, NULL };
	const char *host = tedge_c8y_host();
	int conn, ret, status = 0;

	conn = tedge_http_connect(host, 443, true, 30000);
	if (conn < 0) {
		return conn;
	}
	resp_len = 0;
	bufs->resp[0] = '\0';
	resp_status = 0;
	{
		struct tedge_http_request req = {
			.method = "POST",
			.host = host,
			.path = path,
			.headers = (auth != NULL) ? headers : NULL,
			.content_type = "application/pkcs10",
			.payload = body,
			.body_len = strlen(body),
			.on_body = on_body,
			.buf = bufs->http_rx,
			.buf_len = sizeof(bufs->http_rx),
			.timeout_ms = 30000,
		};

		ret = tedge_http_exchange(conn, &req, &status);
	}
	tedge_port_close(conn);
	resp_status = (uint16_t)status;
	return ret;
}

/* ------------------------------------------------------------------------ */
/* PKCS#7 (certs-only) -> the DER certificate                                */
/* ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ */
/* TLS credentials                                                           */
/* ------------------------------------------------------------------------ */

static int register_credentials(void)
{
	int ret;

	if (credentials_ready) {
		return 0;
	}
	ret = tedge_port_tls_set_device(cert_der, cert_der_len);
	if (ret != 0) {
		TEDGE_LOG_ERR("registering the device credentials failed (%d)",
			      ret);
		return ret;
	}
	credentials_ready = true;
	TEDGE_LOG_INF("device certificate ready (%zu B)", cert_der_len);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Enrollment                                                                */
/* ------------------------------------------------------------------------ */

/* Shared with renewal: POST the CSR in the shared buffer to @p path with
 * @p auth, and unwrap the certificate from the reply into @p out.
 */
int tedge_est_request(const char *path, const char *auth, uint8_t *out,
		      size_t cap, size_t *out_len)
{
	int ret;

	if (bufs == NULL) {
		return -EINVAL;
	}
	ret = https_post(path, auth, bufs->csr_body);
	if (ret != 0) {
		return ret;
	}
	if (resp_status != 200) {
		TEDGE_LOG_WRN("EST %s: the server answered %u", path, resp_status);
		return -EACCES;
	}
	return tedge_pkcs7_first_cert(bufs->resp, bufs->csr_body,
				      sizeof(bufs->csr_body), bufs->der,
				      sizeof(bufs->der), out, cap, out_len);
}

/* Shared with renewal: drop the transient buffers once a flow is done. */
void tedge_est_release(void)
{
	tedge_free(bufs);
	bufs = NULL;
}

/* Shared with renewal: store a new certificate and use it from now on. */
int tedge_credentials_replace(const uint8_t *der, size_t len)
{
	if (len == 0 || len > sizeof(cert_der)) {
		return -EINVAL;
	}
	memcpy(cert_der, der, len);
	cert_der_len = len;
	(void)tedge_port_kv_set(TEDGE_KEY_ENROLL_CERT, cert_der, cert_der_len);
	credentials_ready = false; /* re-register under the same tag */
	return register_credentials();
}

/* The certificate in use, for the expiry check. */
const uint8_t *tedge_credentials_cert(size_t *len)
{
	*len = cert_der_len;
	return (cert_der_len > 0) ? cert_der : NULL;
}

static int enroll_once(void)
{
	/* external ID (<= 63) + ':' + password (<= OTP_MAX) */
	char basic[64 + 1 + OTP_MAX + 1];
	char basic_b64[BASE64_LEN(sizeof(basic) - 1) + 1];
	char auth_hdr[sizeof("Authorization: Basic \r\n") + sizeof(basic_b64)];
	int ret;

	ret = tedge_basic_credential(basic, sizeof(basic),
				     tedge_identity()->external_id, otp);
	if (ret < 0) {
		TEDGE_LOG_ERR("external ID and password are too long for a credential");
		return ret;
	}
	ret = tedge_base64_encode((const uint8_t *)basic, strlen(basic),
				  basic_b64, sizeof(basic_b64));
	if (ret < 0) {
		return -EINVAL;
	}
	snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Basic %s\r\n",
		 basic_b64);

	ret = https_post(EST_PATH, auth_hdr, bufs->csr_body);
	if (ret != 0) {
		TEDGE_LOG_WRN("enrollment request failed (%d)", ret);
		return ret;
	}
	if (resp_status != 200) {
		/* 401 until an operator registers the external ID. */
		TEDGE_LOG_DBG("not registered yet (HTTP %u)", resp_status);
		return -EAGAIN;
	}
	ret = tedge_pkcs7_first_cert(bufs->resp, bufs->csr_body,
				     sizeof(bufs->csr_body), bufs->der,
				     sizeof(bufs->der), cert_der,
				     sizeof(cert_der), &cert_der_len);
	if (ret != 0) {
		TEDGE_LOG_ERR("could not read the certificate from the reply (%d)", ret);
		return ret;
	}
	(void)tedge_port_kv_set(TEDGE_KEY_ENROLL_CERT, cert_der, cert_der_len);
	(void)tedge_port_kv_delete(TEDGE_KEY_ENROLL_OTP);
	(void)tedge_port_kv_delete(TEDGE_KEY_ENROLL_OTP_EXT);
	memset(otp, 0, sizeof(otp));
	TEDGE_LOG_INF("enrolled: certificate stored (%zu B)", cert_der_len);
	return 0;
}

int tedge_auth_prepare(char *id_out, size_t id_len)
{
	int64_t deadline;
	uint32_t wait_s = TEDGE_ENROLL_POLL_S;
	int ret;

	snprintf(id_out, id_len, "%s", tedge_identity()->external_id);
	if (credentials_ready) {
		return 0;
	}

	if (cert_der_len == 0) {
		cert_der_len = stored(TEDGE_KEY_ENROLL_CERT, cert_der,
					    sizeof(cert_der));
	}
	ret = tedge_port_device_key_ensure();
	if (ret != 0) {
		return ret;
	}
	if (cert_der_len > 0) {
		/* Enrolled: any password still stored is a leftover of that
		 * enrollment (a newly supplied one would have discarded the
		 * certificate, see tedge_set_enroll_otp()). */
		(void)tedge_port_kv_delete(TEDGE_KEY_ENROLL_OTP);
		(void)tedge_port_kv_delete(TEDGE_KEY_ENROLL_OTP_EXT);
		memset(otp, 0, sizeof(otp));
		return register_credentials();
	}

	/* Enrolling: the transient buffers come from the module heap. */
	bufs = tedge_alloc(sizeof(*bufs));
	if (bufs == NULL) {
		return -ENOMEM;
	}
	ensure_otp();
	tedge_set_state(TEDGE_STATE_AWAITING_REGISTRATION);
	log_registration_hint();

	ret = make_csr_internal();
	deadline = tedge_port_uptime_ms() + ENROLL_WINDOW_MS;
	while (ret == 0 && tedge_port_uptime_ms() < deadline) {
		ret = enroll_once();
		if (ret == 0) {
			break;
		}
		if (tedge_wait_for_stop(wait_s * 1000)) {
			ret = -ECANCELED;
			break;
		}
		wait_s = TEDGE_MIN(wait_s * 2, 60U);
		ret = -EAGAIN;
	}
	if (ret == 0) {
		ret = register_credentials();
	}
	tedge_free(bufs);
	bufs = NULL;
	return ret;
}

int tedge_set_bootstrap_credentials(const char *user, const char *password)
{
	TEDGE_UNUSED(user);
	TEDGE_UNUSED(password);
	return -ENOTSUP; /* this build authenticates with a certificate */
}

const char *tedge_auth_username(void)
{
	return NULL; /* mutual TLS */
}

const char *tedge_auth_password(void)
{
	return NULL;
}
