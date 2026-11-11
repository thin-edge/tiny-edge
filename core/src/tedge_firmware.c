/* SPDX-License-Identifier: Apache-2.0
 *
 * Firmware update through MCUboot.
 *
 *   515,<device>,<name>,<version>,<url>
 *     -> refuse if that version is already running
 *     -> 501, then download into the secondary slot (own thread)
 *     -> remember the pending version, request a test boot, reset
 *     -> MCUboot swaps; the device is offline for ~40 s (C6) / ~19 s (S3)
 *     -> the new image boots unconfirmed, connects, the application's hook
 *        agrees, and only then is it confirmed: 115 + 503
 *     -> if it never confirms, MCUboot reverts it and the old image reports
 *        502 naming the version that failed
 *
 * The marker in settings is the only state kept across the swap, which is
 * what lets the old image tell "I was reverted" from "I just booted".
 */

#include "tedge_internal.h"
#include "tedge_util.h"

#include <tedge/port/firmware.h>
#include <tedge/port/kv.h>
#include <tedge/port/log.h>
#include <tedge/port/os.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEDGE_LOG_DECLARE();

#define MARKER_SEPARATOR ','

struct pending {
	char name[48];
	char version[24];
	char url[600];
};

static struct pending job;
static int64_t image_total;
static int64_t last_progress_at;
static int last_progress_percent;
static int64_t last_progress_bytes;
static bool job_running;

static tedge_port_thread_t dl_thread;
TEDGE_PORT_STACK_DEFINE(dl_stack, TEDGE_FIRMWARE_STACK_SIZE);

/* Results for the client thread (the MQTT client is not thread-safe). */
TEDGE_PORT_MSGQ_DEFINE(fw_events, sizeof(struct tedge_fw_event), 2);

static void post(enum tedge_fw_event_type type, const char *fmt, ...)
{
	struct tedge_fw_event ev = { .type = type };
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(ev.text, sizeof(ev.text), fmt, ap);
	va_end(ap);
	(void)tedge_port_msgq_put(&fw_events, &ev);
}

int tedge_fw_poll_event(struct tedge_fw_event *ev)
{
	return tedge_port_msgq_get(&fw_events, ev);
}

/* ------------------------------------------------------------------------ */
/* The running image                                                         */
/* ------------------------------------------------------------------------ */

int tedge_fw_running_version(char *buf, size_t len)
{
	char header[16] = "";

	if (tedge_port_fw_running_version(header, sizeof(header)) != 0) {
		header[0] = '\0';
	}
	return tedge_fw_version_pick(tedge_identity()->firmware_version, header,
				     buf, len);
}

/* ------------------------------------------------------------------------ */
/* Progress (free-form topics only, QoS 0)                                   */
/* ------------------------------------------------------------------------ */

static void publish_progress(const char *phase, int percent, int64_t bytes,
			     const char *reason)
{
#if defined(TEDGE_FIRMWARE_PROGRESS)
	char json[240];
	size_t n;

	n = tedge_appendf(json, sizeof(json), 0,
			  "{\"name\":\"%s\",\"version\":\"%s\",\"phase\":\"%s\"",
			  job.name, job.version, phase);
	if (percent >= 0) {
		n = tedge_appendf(json, sizeof(json), n, ",\"percent\":%d",
				  percent);
	}
	n = tedge_appendf(json, sizeof(json), n, ",\"bytes\":%lld",
			  (long long)bytes);
	if (image_total > 0) {
		n = tedge_appendf(json, sizeof(json), n, ",\"total\":%lld",
				  (long long)image_total);
	}
	if (reason != NULL) {
		n = tedge_appendf(json, sizeof(json), n, ",\"reason\":\"%s\"",
				  reason);
	}
	n = tedge_appendf(json, sizeof(json), n, "}");
	if (n < sizeof(json)) {
		/* At most once: progress is a hint, not state, and it must not
		 * queue up behind device management. */
		(void)tedge_c8y_publish_progress("firmware", json);
	}
#else
	TEDGE_UNUSED(phase);
	TEDGE_UNUSED(percent);
	TEDGE_UNUSED(bytes);
	TEDGE_UNUSED(reason);
#endif
}

/* ------------------------------------------------------------------------ */
/* The marker                                                                */
/* ------------------------------------------------------------------------ */

static char marker[80];


/* The operation that asked for this update finishes after a reboot, so its
 * id is kept beside the marker. It is a separate key on purpose: the marker
 * itself is a contract between firmware versions and must not change shape.
 */
void tedge_fw_remember_operation(const char *id)
{
	if (id == NULL || id[0] == '\0') {
		(void)tedge_port_kv_delete(TEDGE_KEY_FIRMWARE_OP);
		return;
	}
	(void)tedge_port_kv_set(TEDGE_KEY_FIRMWARE_OP, id, strlen(id) + 1);
}

int tedge_fw_operation_id(char *out, size_t len)
{
	int n;

	if (out == NULL || len == 0) {
		return -EINVAL;
	}
	n = tedge_port_kv_get(TEDGE_KEY_FIRMWARE_OP, out, len - 1);
	out[(n > 0) ? n : 0] = '\0';
	if (out[0] == '\0') {
		return -ENOENT;
	}
	/* One operation, one answer: the id goes once the update is over. */
	(void)tedge_port_kv_delete(TEDGE_KEY_FIRMWARE_OP);
	return 0;
}

static void marker_load(void)
{
	int n = tedge_port_kv_get(TEDGE_KEY_FIRMWARE, marker, sizeof(marker) - 1);

	marker[(n > 0) ? n : 0] = '\0';
}

static void marker_save(const char *name, const char *version, int64_t total)
{
	char buf[80];
	int n = snprintf(buf, sizeof(buf), "%s%c%s", name, MARKER_SEPARATOR,
			 version);

	if (n > 0 && n < (int)sizeof(buf)) {
		(void)tedge_port_kv_set(TEDGE_KEY_FIRMWARE, buf, n);
	}
	/* The size travels in its own key: an older image on the other side of
	 * the swap parses the marker, and its format must stay as it was. */
	n = snprintf(buf, sizeof(buf), "%lld", (long long)total);
	if (n > 0 && n < (int)sizeof(buf)) {
		(void)tedge_port_kv_set(TEDGE_KEY_FIRMWARE_SIZE, buf, n);
	}
}

static void marker_clear(void)
{
	marker[0] = '\0';
	(void)tedge_port_kv_delete(TEDGE_KEY_FIRMWARE);
	(void)tedge_port_kv_delete(TEDGE_KEY_FIRMWARE_SIZE);
}

/* The size of the image that was installed, written beside the marker. */
static char size_text[24];

static int64_t size_load(void)
{
	int n = tedge_port_kv_get(TEDGE_KEY_FIRMWARE_SIZE, size_text,
				  sizeof(size_text) - 1);

	size_text[(n > 0) ? n : 0] = '\0';
	return (size_text[0] != '\0') ? strtoll(size_text, NULL, 10) : 0;
}

/* ------------------------------------------------------------------------ */
/* The download                                                              */
/* ------------------------------------------------------------------------ */

static int write_sink(const void *data, size_t len, void *user_data)
{
	TEDGE_UNUSED(user_data);
	int ret = tedge_port_fw_write(data, len);

	if (ret != 0) {
		TEDGE_LOG_ERR("firmware: writing to the slot failed (%d)", ret);
	}
	return ret;
}

/* The transfer in flight, so the progress callback can see its length. */
static struct tedge_download dl_req;

/* Cumulocity serves binaries chunked, so there is usually no Content-Length
 * and no percentage to report; then progress is measured in bytes instead.
 */
#define PROGRESS_BYTES_STEP (128 * 1024)

static void report_progress(int64_t written, void *user_data)
{
	int percent = -1;

	TEDGE_UNUSED(user_data);
	bool first = (last_progress_at == 0);

	image_total = dl_req.total;
	if (image_total > 0) {
		percent = (int)((written * 100) / image_total);
		if (!first &&
		    percent <
			    last_progress_percent + TEDGE_FIRMWARE_PROGRESS_PERCENT) {
			return;
		}
	} else if (!first && written < last_progress_bytes + PROGRESS_BYTES_STEP) {
		return;
	}
	if (!first && tedge_port_uptime_ms() - last_progress_at < 1000) {
		return; /* at most one a second */
	}
	last_progress_percent = (percent >= 0) ? percent : 0;
	last_progress_bytes = written;
	last_progress_at = tedge_port_uptime_ms();
	publish_progress("downloading", percent, written, NULL);
}

static void download_thread(void *arg)
{
	struct tedge_download req = {
		.url = job.url,
		.token = tedge_c8y_jwt(),
		.sink = write_sink,
		.progress = report_progress,
		.timeout_ms = 120000,
	};
	int64_t t0 = tedge_port_uptime_ms();
	int ret;

	dl_req = req;
	TEDGE_UNUSED(arg);

	last_progress_percent = 0;
	last_progress_bytes = 0;
	last_progress_at = 0;
	image_total = 0;
	/* The first message waits for the response headers, so that it can
	 * state the size like every other one. */

	ret = tedge_port_fw_begin();
	if (ret == 0) {
		ret = tedge_download(&dl_req);
	}
	if (ret == 0) {
		ret = tedge_port_fw_finish();
	}
	if (ret != 0) {
		publish_progress("failed", -1, 0, "download failed");
		post(TEDGE_FW_FAILED, "download failed (%d)", ret);
		job_running = false;
		return;
	}

	TEDGE_LOG_INF("firmware: %lld B written in %lld s", (long long)dl_req.written,
		(long long)((tedge_port_uptime_ms() - t0) / 1000));
	publish_progress("installing", 100, dl_req.written, NULL);

	/* From here the device is about to go dark for the swap. */
	marker_save(job.name, job.version, dl_req.total ? dl_req.total
							: dl_req.written);
	ret = tedge_port_fw_request_trial();
	if (ret != 0) {
		marker_clear();
		publish_progress("failed", -1, 0, "could not request the update");
		post(TEDGE_FW_FAILED, "could not request the update (%d)", ret);
		job_running = false;
		return;
	}
	/* The new image reports the operation, through the marker: it was
	 * not interrupted. */
	(void)tedge_port_kv_delete(TEDGE_KEY_OP_RUNNING);
	post(TEDGE_FW_REBOOTING, "installing %s", job.version);
	tedge_port_sleep_ms(1500); /* let the report and the progress leave */
	tedge_device_reset();
}

/* ------------------------------------------------------------------------ */
/* The confirm deadline                                                      */
/* ------------------------------------------------------------------------ */

/* A test-booted image that never reaches the cloud would otherwise run for
 * ever: the network is up, so nothing else resets the device and the
 * bootloader never gets to roll it back. */
static void confirm_deadline_fn(void)
{
	if (tedge_port_fw_is_confirmed()) {
		return;
	}
	TEDGE_LOG_ERR("firmware: this image has not confirmed itself within %d s; "
		"resetting so the bootloader can roll it back",
		TEDGE_FIRMWARE_CONFIRM_TIMEOUT_S);
	tedge_device_reset();
}

TEDGE_PORT_DELAYED_WORK_DEFINE(confirm_deadline, confirm_deadline_fn);

void tedge_fw_arm_confirm_deadline(void)
{
	if (TEDGE_FIRMWARE_CONFIRM_TIMEOUT_S == 0 ||
	    tedge_port_fw_is_confirmed()) {
		return;
	}
	TEDGE_LOG_INF("firmware: running an unconfirmed image; it has %d s to "
		"confirm itself", TEDGE_FIRMWARE_CONFIRM_TIMEOUT_S);
	tedge_port_delayed_work_schedule(
		&confirm_deadline, TEDGE_FIRMWARE_CONFIRM_TIMEOUT_S * 1000);
}

/* ------------------------------------------------------------------------ */
/* The operation                                                             */
/* ------------------------------------------------------------------------ */

int tedge_fw_request(const char *line, char *reason, size_t rlen)
{
	char running[24] = "";
	char name[48], version[24], url[600];

	/* 515,<device>,<name>,<version>,<url> */
	if (tedge_sr_field(line, 2, name, sizeof(name)) <= 0 ||
	    tedge_sr_field(line, 3, version, sizeof(version)) <= 0 ||
	    tedge_sr_field(line, 4, url, sizeof(url)) <= 0) {
		snprintf(reason, rlen, "malformed firmware request");
		return -EINVAL;
	}
	if (job_running) {
		TEDGE_LOG_WRN("firmware: refused %s: an update is already running",
			version);
		snprintf(reason, rlen, "an update is already running");
		return -EBUSY;
	}

	/* Re-installing what is already running costs a download and the
	 * downtime of a swap, for no change. */
	(void)tedge_fw_running_version(running, sizeof(running));
	if (tedge_fw_is_running(tedge_identity()->firmware_name, running, name,
				version)) {
		TEDGE_LOG_INF("firmware: refused %s %s: it is already running", name,
			version);
		snprintf(reason, rlen, "%s %s is already running", name, version);
		return -EALREADY;
	}

	snprintf(job.name, sizeof(job.name), "%s", name);
	snprintf(job.version, sizeof(job.version), "%s", version);
	if (snprintf(job.url, sizeof(job.url), "%s", url) >= (int)sizeof(job.url)) {
		snprintf(reason, rlen, "the download URL is too long");
		return -ENOSPC;
	}

	TEDGE_LOG_INF("firmware: installing %s %s", job.name, job.version);
	job_running = true;
	(void)tedge_port_thread_start(&dl_thread, dl_stack,
				      TEDGE_PORT_STACK_SIZEOF(dl_stack),
				      download_thread, NULL, "tedge_fw");
	return 0;
}

/* How long the device will be offline while the bootloader swaps, for the
 * operator to see in the operation. */
const char *tedge_fw_downtime_hint(void)
{
	return "the device will be offline for up to a minute while it installs";
}

/* Called from the client thread once the session is up. Confirms a test
 * boot, or reports that a previous image was reverted. */
void tedge_fw_on_connected(void)
{
	const struct tedge_hooks *hooks = tedge_hook_table();
	char version[24] = "";
	bool confirmed = tedge_port_fw_is_confirmed();
	char *sep;

	if (marker[0] == '\0') {
		marker_load();
	}
	if (marker[0] == '\0') {
		return; /* nothing pending */
	}
	sep = strchr(marker, MARKER_SEPARATOR);
	if (sep != NULL) {
		snprintf(job.name, sizeof(job.name), "%.*s",
			 (int)(sep - marker), marker);
		snprintf(job.version, sizeof(job.version), "%s", sep + 1);
	}
	image_total = size_load();
	(void)tedge_fw_running_version(version, sizeof(version));

	switch (tedge_fw_boot_outcome(confirmed, version, job.version)) {
	case TEDGE_FW_BOOT_REVERTED:
		/* We are running a confirmed image that is not the one that was
		 * installed: MCUboot reverted it. */
		TEDGE_LOG_WRN("firmware: %s did not come up; running %s", job.version,
			version);
		post(TEDGE_FW_REVERTED,
		     "%s was rolled back; the device is running %s", job.version,
		     version);
		publish_progress("failed", -1, image_total, "rolled back");
		marker_clear();
		return;
	case TEDGE_FW_BOOT_DONE:
		marker_clear(); /* already confirmed, nothing to do */
		return;
	case TEDGE_FW_BOOT_TEST:
		break;
	}

	if (hooks != NULL && hooks->firmware_confirm_check != NULL) {
		int rc = hooks->firmware_confirm_check(hooks->user_data);

		if (rc != 0) {
			TEDGE_LOG_WRN("firmware: the application refused %s (%d); it "
				"will be rolled back on the next reset",
				job.version, rc);
			post(TEDGE_FW_FAILED,
			     "the application refused %s; it will be rolled back",
			     job.version);
			publish_progress("failed", -1, 0,
					 "the application refused it");
			return; /* leave it unconfirmed on purpose */
		}
	}
	if (TEDGE_IS_ENABLED(TEDGE_FIRMWARE_CONFIRM_AFTER_CONNECT)) {
		int rc = tedge_port_fw_confirm();

		if (rc != 0) {
			TEDGE_LOG_ERR("firmware: could not confirm the image (%d)", rc);
			return;
		}
		tedge_port_delayed_work_cancel(&confirm_deadline);
		TEDGE_LOG_INF("firmware: %s confirmed", job.version);
		post(TEDGE_FW_INSTALLED, "%s", job.version);
		publish_progress("done", 100, image_total, NULL);
		marker_clear();
	}
}
