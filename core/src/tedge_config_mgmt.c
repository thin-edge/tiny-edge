/* SPDX-License-Identifier: Apache-2.0
 *
 * Cumulocity configuration management: the device's configuration files,
 * read ("get", c8y_UploadConfigFile) and replaced ("set",
 * c8y_DownloadConfigFile) from the cloud.
 *
 * A configuration type is a file the client knows how to write out and to
 * take in, piece by piece, so no file is ever held whole in RAM. The types
 * are compiled in (the table below); the device advertises exactly those in
 * c8y_SupportedConfigurations.
 *
 *   get: the file is produced twice (measured, then sent, as log upload
 *        does), attached to a new event as its binary, and the operation
 *        finishes with the binary's URL.
 *   set: the file is downloaded from the URL in the operation (the tenant's
 *        binary, or anywhere the download rules allow) into the type's
 *        writer; the operation finishes only when the type has checked and
 *        stored it, and with its reason when it has not.
 *
 * The work runs on a thread of its own, one request at a time. The HTTP
 * client keeps its buffers in static storage, so a transfer never runs
 * beside a firmware download or a log upload either.
 */

#include "tedge_internal.h"
#include "tedge_util.h"

#include <tedge/port/log.h>
#include <tedge/port/os.h>
#include <tedge/port/time.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

TEDGE_LOG_DECLARE();

/* ------------------------------------------------------------------------ */
/* The types                                                                 */
/* ------------------------------------------------------------------------ */

struct config_type {
	const char *name;
	/* get: writes the file through the sink; the same bytes each time. */
	int (*read)(tedge_sink_fn sink, void *ctx);
	/* set: begin, chunks, then end (or abort). end checks and stores, or
	 * fills the reason. */
	int (*write_begin)(void);
	int (*write_chunk)(const void *data, size_t len);
	int (*write_end)(char *reason, size_t cap);
	void (*write_abort)(void);
};

static const struct config_type types[] = {
#if defined(TEDGE_TRUSTED_ROOTS)
	{
		.name = "trusted-roots",
		.read = tedge_trust_read,
		.write_begin = tedge_trust_write_begin,
		.write_chunk = tedge_trust_write_chunk,
		.write_end = tedge_trust_write_end,
		.write_abort = tedge_trust_write_abort,
	},
#endif
	/* Keeps the table valid when no type is compiled in. */
	{ .name = NULL },
};

static const struct config_type *type_named(const char *name)
{
	for (size_t i = 0; i < TEDGE_ARRAY_SIZE(types); i++) {
		if (types[i].name != NULL && strcmp(types[i].name, name) == 0) {
			return &types[i];
		}
	}
	return NULL;
}

size_t tedge_config_types_line(char *out, size_t len)
{
	size_t n = tedge_appendf(out, len, 0, "119");

	for (size_t i = 0; i < TEDGE_ARRAY_SIZE(types) && n < len; i++) {
		if (types[i].name != NULL) {
			n = tedge_appendf(out, len, n, ",%s", types[i].name);
		}
	}
	/* A line cut short would offer a type that does not exist. */
	return (n > 3 && n < len) ? n : 0;
}

/* ------------------------------------------------------------------------ */
/* The job                                                                   */
/* ------------------------------------------------------------------------ */

TEDGE_PORT_STACK_DEFINE(cfg_stack, TEDGE_CONFIG_MGMT_STACK_SIZE);
static tedge_port_thread_t cfg_thread;
static volatile bool job_running;

static struct {
	bool upload; /* get; otherwise set */
	const struct config_type *type;
	char op_id[24];
	char url[TEDGE_URL_MAX];
	size_t received;
	int write_rc; /* the writer's refusal while downloading */
} job;

TEDGE_PORT_MSGQ_DEFINE(cfg_events, sizeof(struct tedge_config_event), 2);

int tedge_config_poll_event(struct tedge_config_event *ev)
{
	return tedge_port_msgq_get(&cfg_events, ev);
}

bool tedge_config_busy(void)
{
	return job_running;
}

static void post(int rc, const char *text)
{
	struct tedge_config_event ev = { .rc = rc };

	snprintf(ev.op_name, sizeof(ev.op_name), "%s",
		 job.upload ? "c8y_UploadConfigFile" : "c8y_DownloadConfigFile");
	snprintf(ev.op_id, sizeof(ev.op_id), "%s", job.op_id);
	snprintf(ev.text, sizeof(ev.text), "%s", (text != NULL) ? text : "");
	(void)tedge_port_msgq_put(&cfg_events, &ev);
}

/* ------------------------------------------------------------------------ */
/* get                                                                       */
/* ------------------------------------------------------------------------ */

/* The device knows its external ID; the event needs its managed-object id.
 * One small GET per session answers that, as for log upload. */
static char mo_id[24];

struct grab {
	char *buf;
	size_t len;
	size_t used;
};

static int grab_sink(const void *data, size_t len, void *ctx)
{
	struct grab *g = ctx;
	size_t n = TEDGE_MIN(len, g->len - 1 - g->used);

	memcpy(g->buf + g->used, data, n);
	g->used += n;
	g->buf[g->used] = '\0';
	return 0;
}

static int resolve_mo_id(void)
{
	char body[320];
	char url[256];
	struct grab g = { .buf = body, .len = sizeof(body) };
	struct tedge_download req = { .url = url,
				      .token = tedge_c8y_jwt(),
				      .sink = grab_sink,
				      .user_data = &g,
				      .timeout_ms = 15000 };
	int ret;

	if (mo_id[0] != '\0') {
		return 0;
	}
	snprintf(url, sizeof(url),
		 "https://%s/identity/externalIds/c8y_Serial/%s",
		 tedge_c8y_host(), tedge_identity()->external_id);
	ret = tedge_download(&req);
	if (ret != 0) {
		return ret;
	}
	if (tedge_json_field(body, "id", mo_id, sizeof(mo_id)) <= 0) {
		return -ENOENT;
	}
	return 0;
}

static int produce(tedge_sink_fn sink, void *ctx, void *user_data)
{
	TEDGE_UNUSED(user_data);
	return job.type->read(sink, ctx);
}

static int count_sink(const void *data, size_t len, void *ctx)
{
	TEDGE_UNUSED(data);
	*(size_t *)ctx += len;
	return 0;
}

static void do_upload(void)
{
	char payload[224], when[24], endpoint[160];
	char event_url[192], binaries[224], filename[112];
	size_t length = 0;
	time_t now = 0;
	struct tm tm;
	int ret;

	ret = resolve_mo_id();
	if (ret != 0) {
		post(ret, "the device could not resolve its own id");
		return;
	}
	(void)produce(count_sink, &length, NULL);

	/* The event the file hangs off: of the configuration's type, as
	 * Cumulocity's configuration snapshots are. */
	(void)tedge_port_time_now(&now);
	gmtime_r(&now, &tm);
	strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
	snprintf(payload, sizeof(payload),
		 "{\"source\":{\"id\":\"%s\"},\"type\":\"%s\","
		 "\"text\":\"Configuration %s\",\"time\":\"%s\"}",
		 mo_id, job.type->name, job.type->name, when);
	snprintf(endpoint, sizeof(endpoint), "https://%s/event/events",
		 tedge_c8y_host());
	{
		struct tedge_upload up = { .url = endpoint,
					   .token = tedge_c8y_jwt(),
					   .content_type = "application/json",
					   .payload = payload,
					   .location = event_url,
					   .location_len = sizeof(event_url),
					   .timeout_ms = 20000 };

		ret = tedge_upload(&up);
	}
	if (ret != 0 || event_url[0] == '\0') {
		post((ret != 0) ? ret : -EIO,
		     "the device could not create the configuration's event");
		return;
	}
	snprintf(binaries, sizeof(binaries), "%s/binaries", event_url);
	snprintf(filename, sizeof(filename), "%s-%s.pem", job.type->name,
		 tedge_identity()->external_id);
	{
		struct tedge_upload up = { .url = binaries,
					   .token = tedge_c8y_jwt(),
					   .content_type = "text/plain",
					   .filename = filename,
					   .producer = produce,
					   .length = length,
					   .timeout_ms = 60000 };

		ret = tedge_upload(&up);
	}
	if (ret != 0) {
		post(ret, "the configuration could not be uploaded");
		return;
	}
	TEDGE_LOG_INF("config: sent %s (%zu B)", job.type->name, length);
	post(0, binaries);
}

/* ------------------------------------------------------------------------ */
/* set                                                                       */
/* ------------------------------------------------------------------------ */

static int write_sink(const void *data, size_t len, void *ctx)
{
	TEDGE_UNUSED(ctx);
	job.received += len;
	if (job.received > TEDGE_CONFIG_MAX_BYTES) {
		job.write_rc = -EFBIG;
		return -EFBIG;
	}
	job.write_rc = job.type->write_chunk(data, len);
	return job.write_rc;
}

static void do_download(void)
{
	char reason[160];
	struct tedge_download req = { .url = job.url,
				      .token = tedge_c8y_jwt(),
				      .sink = write_sink,
				      .timeout_ms = 30000 };
	int ret = job.type->write_begin();

	if (ret != 0) {
		post(ret, (ret == -ENOMEM) ? "out of memory"
					   : "the device is busy");
		return;
	}
	ret = tedge_download(&req);
	if (ret != 0 && job.write_rc == -EFBIG) {
		job.type->write_abort();
		snprintf(reason, sizeof(reason), "the file is larger than the "
			 "%u bytes this device accepts",
			 (unsigned int)TEDGE_CONFIG_MAX_BYTES);
		post(-EFBIG, reason);
		return;
	}
	if (ret != 0 && job.write_rc == 0) {
		job.type->write_abort();
		snprintf(reason, sizeof(reason), "download failed (%d)", ret);
		post(ret, reason);
		return;
	}
	/* A refusal by the writer, or a whole file: end says which. */
	reason[0] = '\0';
	ret = job.type->write_end(reason, sizeof(reason));
	if (ret != 0) {
		post(ret, reason[0] != '\0' ? reason : "the file was refused");
		return;
	}
	TEDGE_LOG_INF("config: %s replaced (%zu B)", job.type->name,
		      job.received);
	post(0, NULL);
}

static void config_thread(void *arg)
{
	TEDGE_UNUSED(arg);
	if (job.upload) {
		do_upload();
	} else {
		do_download();
	}
	job_running = false;
}

/* ------------------------------------------------------------------------ */
/* The requests                                                              */
/* ------------------------------------------------------------------------ */

static int start(bool upload, const char *type, const char *url,
		 const char *op_id, char *reason, size_t rlen)
{
	const struct config_type *t = type_named(type);

	if (t == NULL) {
		snprintf(reason, rlen, "unknown configuration type %s", type);
		return -ENOENT;
	}
	if (job_running) {
		snprintf(reason, rlen, "busy: a configuration request is "
			 "already running");
		return -EBUSY;
	}
	/* The HTTP client's buffers are static: one transfer at a time. */
#if defined(TEDGE_FIRMWARE_UPDATE)
	if (tedge_fw_busy()) {
		snprintf(reason, rlen, "busy: a firmware update is running");
		return -EBUSY;
	}
#endif
#if defined(TEDGE_LOG_UPLOAD)
	if (tedge_log_busy()) {
		snprintf(reason, rlen, "busy: a log is being sent");
		return -EBUSY;
	}
#endif
	memset(&job, 0, sizeof(job));
	job.upload = upload;
	job.type = t;
	snprintf(job.op_id, sizeof(job.op_id), "%s",
		 (op_id != NULL) ? op_id : "");
	if (url != NULL &&
	    snprintf(job.url, sizeof(job.url), "%s", url) >= (int)sizeof(job.url)) {
		snprintf(reason, rlen, "the download URL is too long");
		return -ENOSPC;
	}
	TEDGE_LOG_INF("config: %s %s", upload ? "sending" : "replacing",
		      t->name);
	job_running = true;
	if (tedge_port_thread_start(&cfg_thread, cfg_stack,
				    TEDGE_PORT_STACK_SIZEOF(cfg_stack),
				    config_thread, NULL, "tedge_cfg") != 0) {
		job_running = false;
		snprintf(reason, rlen, "the device could not start the work");
		return -ENOMEM;
	}
	return 0;
}

int tedge_config_request_upload(const char *line, const char *op_id,
				char *reason, size_t rlen)
{
	char type[48];

	/* 526,<device>,<type> */
	if (tedge_sr_field(line, 2, type, sizeof(type)) <= 0) {
		snprintf(reason, rlen, "malformed configuration request");
		return -EINVAL;
	}
	return start(true, type, NULL, op_id, reason, rlen);
}

int tedge_config_request_download(const char *line, const char *op_id,
				  char *reason, size_t rlen)
{
	char type[48], url[TEDGE_URL_MAX];

	/* 524,<device>,<url>,<type> */
	if (tedge_sr_field(line, 2, url, sizeof(url)) <= 0 ||
	    tedge_sr_field(line, 3, type, sizeof(type)) <= 0) {
		snprintf(reason, rlen, "malformed configuration request");
		return -EINVAL;
	}
	return start(false, type, url, op_id, reason, rlen);
}
