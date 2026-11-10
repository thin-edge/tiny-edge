/* SPDX-License-Identifier: Apache-2.0
 *
 * The provisioning library (include/tedge/provisioning.h) over Improv
 * Serial, with the real client on the POSIX port: every command, damaged
 * and split frames, log text between frames, and the onboarding hand-over
 * to the client.
 */

#include <tedge/port/os.h>
#include <tedge/posix.h>
#include <tedge/provisioning.h>
#include <tedge/tedge.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* --- What the device writes ---------------------------------------------- */

static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t out[8192];
static size_t out_len;

static void capture(const void *data, size_t len, void *ctx)
{
	(void)ctx;
	pthread_mutex_lock(&out_lock);
	if (out_len + len <= sizeof(out)) {
		memcpy(out + out_len, data, len);
		out_len += len;
	}
	pthread_mutex_unlock(&out_lock);
}

struct frame {
	uint8_t type;
	uint8_t len;
	uint8_t data[255];
};

/* The @p index-th valid frame the device has written, if there is one. */
static bool frame_at(size_t index, struct frame *f)
{
	size_t found = 0;
	bool ok = false;

	pthread_mutex_lock(&out_lock);
	for (size_t i = 0; i + 10 <= out_len; i++) {
		uint8_t sum = 0;
		size_t len;

		if (memcmp(out + i, "IMPROV", 6) != 0 || out[i + 6] != 1) {
			continue;
		}
		len = out[i + 8];
		if (i + 9 + len + 1 > out_len) {
			break;
		}
		for (size_t k = 0; k < 9 + len; k++) {
			sum += out[i + k];
		}
		if (sum != out[i + 9 + len]) {
			continue;
		}
		if (found++ == index) {
			f->type = out[i + 7];
			f->len = (uint8_t)len;
			memcpy(f->data, out + i + 9, len);
			ok = true;
			break;
		}
		i += 9 + len;
	}
	pthread_mutex_unlock(&out_lock);
	return ok;
}

static size_t next_frame;

/* Wait for the device's next frame. */
static bool expect(struct frame *f)
{
	for (int t = 0; t < 400; t++) {
		if (frame_at(next_frame, f)) {
			next_frame++;
			return true;
		}
		tedge_port_sleep_ms(10);
	}
	return false;
}

/* The @p i-th string of an RPC result frame, NUL-terminated into @p s. */
static bool result_string(const struct frame *f, int i, char *s, size_t cap)
{
	size_t pos = 2; /* after [command][length] */

	for (int k = 0; pos < f->len; k++) {
		size_t n = f->data[pos++];

		if (pos + n > f->len || n >= cap) {
			return false;
		}
		if (k == i) {
			memcpy(s, f->data + pos, n);
			s[n] = '\0';
			return true;
		}
		pos += n;
	}
	return false;
}

/* --- What a tool sends --------------------------------------------------- */

static size_t encode(uint8_t *buf, uint8_t cmd, const uint8_t *data,
		     size_t len)
{
	uint8_t sum = 0;
	size_t n = 0;

	memcpy(buf, "IMPROV", 6);
	n = 6;
	buf[n++] = 1;           /* version */
	buf[n++] = 0x03;        /* RPC command */
	buf[n++] = (uint8_t)(len + 2);
	buf[n++] = cmd;
	buf[n++] = (uint8_t)len;
	memcpy(buf + n, data, len);
	n += len;
	for (size_t i = 0; i < n; i++) {
		sum += buf[i];
	}
	buf[n++] = sum;
	return n;
}

static void send_rpc(uint8_t cmd, const uint8_t *data, size_t len)
{
	uint8_t buf[300];
	size_t n = encode(buf, cmd, data, len);

	CHECK(tedge_provisioning_serial_feed(buf, n) == 1);
}

/* Length-prefixed strings, as Improv's payloads carry them. */
static size_t strings(uint8_t *buf, const char *a, const char *b)
{
	size_t n = 0;

	buf[n++] = (uint8_t)strlen(a);
	memcpy(buf + n, a, strlen(a));
	n += strlen(a);
	if (b != NULL) {
		buf[n++] = (uint8_t)strlen(b);
		memcpy(buf + n, b, strlen(b));
		n += strlen(b);
	}
	return n;
}

/* --- The application's hooks --------------------------------------------- */

static int wifi_rc;
static char wifi_ssid[40], wifi_password[80];
static int identified;

static int wifi_connect(const char *ssid, const char *password, void *ctx)
{
	(void)ctx;
	snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", ssid);
	snprintf(wifi_password, sizeof(wifi_password), "%s", password);
	return wifi_rc;
}

static void identify(void *ctx)
{
	(void)ctx;
	identified++;
}

static const struct tedge_provisioning_hooks hooks = {
	.wifi_connect = wifi_connect,
	.identify = identify,
	.chip = "host",
};

int main(void)
{
	char dir[] = "/tmp/tedge-provisioning-test-XXXXXX";
	struct tedge_posix_config cfg = { 0 };
	struct tedge_identity id = { .external_id = "tedge-prov-test" };
	struct frame f;
	uint8_t payload[200];
	char s[80];
	size_t n;

	if (mkdtemp(dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	cfg.state_dir = dir;
	CHECK(tedge_posix_configure(&cfg) == 0);
	CHECK(tedge_init(&id, NULL) == 0);
	CHECK(tedge_provisioning_init(&hooks) == 0);
	CHECK(tedge_provisioning_init(&hooks) == -EALREADY);
	tedge_provisioning_serial_attach(capture, NULL);

	/* Improv's device info: firmware, version, chip, the device's ID. */
	send_rpc(0x03, NULL, 0);
	CHECK(expect(&f) && f.type == 0x04 && f.data[0] == 0x03);
	CHECK(result_string(&f, 2, s, sizeof(s)) && strcmp(s, "host") == 0);
	CHECK(result_string(&f, 3, s, sizeof(s)) && strcmp(s, "tedge-prov-test") == 0);

	/* tiny-edge's info, before any tenant is set. */
	send_rpc(TEDGE_PROVISIONING_CMD_INFO, NULL, 0);
	CHECK(expect(&f) && f.type == 0x04 && f.data[0] == 0xE0);
	CHECK(result_string(&f, 0, s, sizeof(s)) && strcmp(s, "1") == 0);
	CHECK(result_string(&f, 1, s, sizeof(s)) && strcmp(s, "tedge-prov-test") == 0);
	CHECK(result_string(&f, 2, s, sizeof(s)) && strcmp(s, "c8y-ca") == 0);
	CHECK(result_string(&f, 3, s, sizeof(s)) && strcmp(s, "") == 0);
	CHECK(result_string(&f, 4, s, sizeof(s)) && strcmp(s, "stopped") == 0);

	/* A command nobody knows. */
	send_rpc(0x7F, NULL, 0);
	CHECK(expect(&f) && f.type == 0x02 && f.data[0] == 0x02);

	/* Log text around a frame that arrives a byte at a time. */
	{
		const char *noise = "<inf> tedge: IMPRO something IMPROV\x02 more\r\n";
		uint8_t buf[64];

		n = encode(buf, TEDGE_PROVISIONING_CMD_IDENTIFY, NULL, 0);
		CHECK(tedge_provisioning_serial_feed(noise, strlen(noise)) == 0);
		for (size_t i = 0; i + 1 < n; i++) {
			CHECK(tedge_provisioning_serial_feed(&buf[i], 1) == 0);
		}
		CHECK(tedge_provisioning_serial_feed(&buf[n - 1], 1) == 1);
		CHECK(expect(&f) && f.type == 0x04 && f.data[0] == 0xE2);
		CHECK(identified == 1);
	}

	/* A damaged frame: answered "invalid RPC", and nothing happens. */
	{
		uint8_t buf[64];

		n = encode(buf, TEDGE_PROVISIONING_CMD_IDENTIFY, NULL, 0);
		buf[n - 1] ^= 0x55;
		CHECK(tedge_provisioning_serial_feed(buf, n) == 0);
		CHECK(expect(&f) && f.type == 0x02 && f.data[0] == 0x01);
		CHECK(identified == 1);
	}

	/* Wi-Fi: joined (the host's network is up). */
	wifi_rc = 0;
	n = strings(payload, "lab-net", "secret-pw");
	send_rpc(0x01, payload, n);
	CHECK(expect(&f) && f.type == 0x01 && f.data[0] == 0x03); /* provisioning */
	CHECK(expect(&f) && f.type == 0x01 && f.data[0] == 0x04); /* provisioned */
	CHECK(expect(&f) && f.type == 0x04 && f.data[0] == 0x01);
	CHECK(strcmp(wifi_ssid, "lab-net") == 0 &&
	      strcmp(wifi_password, "secret-pw") == 0);

	/* Wi-Fi: the network refuses. */
	wifi_rc = -ECONNREFUSED;
	send_rpc(0x01, payload, n);
	CHECK(expect(&f) && f.type == 0x01 && f.data[0] == 0x03);
	CHECK(expect(&f) && f.type == 0x02 && f.data[0] == 0x03); /* unable */
	CHECK(expect(&f) && f.type == 0x01 && f.data[0] == 0x02); /* ready */

	/* Wi-Fi: no SSID, and a string running past the packet. */
	n = strings(payload, "", "pw");
	send_rpc(0x01, payload, n);
	CHECK(expect(&f) && f.type == 0x02 && f.data[0] == 0x01);
	payload[0] = 50;
	send_rpc(0x01, payload, 4);
	CHECK(expect(&f) && f.type == 0x02 && f.data[0] == 0x01);

	/* Onboarding without a password is refused. */
	payload[0] = 0;
	n = 1 + strings(payload + 1, "127.0.0.1", NULL);
	send_rpc(TEDGE_PROVISIONING_CMD_ONBOARD, payload, n);
	CHECK(expect(&f) && f.type == 0x02 && f.data[0] == 0x01);

	/* Onboarding: the client takes the tenant and starts enrolling. */
	payload[0] = 0;
	n = 1 + strings(payload + 1, "127.0.0.1", "Abcdefghijklmnopqrstuvwx");
	send_rpc(TEDGE_PROVISIONING_CMD_ONBOARD, payload, n);
	CHECK(expect(&f) && f.type == 0x04 && f.data[0] == 0xE1);
	send_rpc(TEDGE_PROVISIONING_CMD_INFO, NULL, 0);
	CHECK(expect(&f) && f.type == 0x04);
	CHECK(result_string(&f, 3, s, sizeof(s)) && strcmp(s, "127.0.0.1") == 0);
	CHECK(result_string(&f, 4, s, sizeof(s)) && strcmp(s, "stopped") != 0);

	/* And again while it enrolls: it stops, takes the new data, starts. */
	send_rpc(TEDGE_PROVISIONING_CMD_ONBOARD, payload, n);
	CHECK(expect(&f) && f.type == 0x04 && f.data[0] == 0xE1);
	CHECK(tedge_stop() == 0);

	{
		char rm[80];

		snprintf(rm, sizeof(rm), "rm -rf %s", dir);
		(void)!system(rm);
	}
	if (failures == 0) {
		printf("provisioning: all checks passed\n");
	}
	return failures == 0 ? 0 : 1;
}
