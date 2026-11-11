/* SPDX-License-Identifier: Apache-2.0
 *
 * Running a command the cloud asked for (c8y_Command), with ESP-IDF's
 * console: the command is one the application registered with
 * esp_console_cmd_register(), run with esp_console_run(), and what it
 * prints is the operation's result.
 *
 * It runs in the client, not in a shell on the network, and on a thread of
 * its own, so a command that takes a while does not hold the connection up.
 * What may run is decided by core/src/tedge_shell_allow_list.c, which
 * refuses everything until the integrator says otherwise.
 *
 * The output is captured through stdout. picolibc's stdout is one stream
 * for every task, so while a command runs it is swapped for a stream that
 * keeps what the command's task prints and passes everything else (other
 * tasks' printf, and ESP_LOG, which prints through stdout) on to the
 * console unchanged.
 */

#include <tedge/port/config.h>
#include <tedge/port/log.h>
#include <tedge/port/os.h>

#include "tedge_internal.h"

#include <esp_console.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

TEDGE_LOG_DECLARE();

TEDGE_PORT_STACK_DEFINE(cmd_stack, TEDGE_SHELL_COMMAND_STACK_SIZE);
static tedge_port_thread_t cmd_thread;
TEDGE_PORT_MSGQ_DEFINE(cmd_events, sizeof(struct tedge_shell_event), 2);

static struct {
	char cmd[160];
	int64_t started;
	bool running;
	bool reported;
} job;

/* ------------------------------------------------------------------------ */
/* Capturing stdout                                                          */
/* ------------------------------------------------------------------------ */

static FILE *console_out;        /* stdout as it was before the command */
static TaskHandle_t capture_task; /* whose output is kept */
static char captured[TEDGE_SHELL_COMMAND_OUTPUT_BYTES];
static size_t captured_len;

static int capture_put(char c, FILE *f)
{
	(void)f;
	if (capture_task != NULL &&
	    xTaskGetCurrentTaskHandle() == capture_task) {
		if (captured_len < sizeof(captured) - 1) {
			captured[captured_len++] = c;
		}
		return (unsigned char)c;
	}
	return fputc(c, console_out);
}

static int capture_flush(FILE *f)
{
	(void)f;
	return fflush(console_out);
}

static FILE capture_stream =
	FDEV_SETUP_STREAM(capture_put, NULL, capture_flush, _FDEV_SETUP_WRITE);

/* ------------------------------------------------------------------------ */
/* Running it                                                                */
/* ------------------------------------------------------------------------ */

static void run_thread(void *arg)
{
	struct tedge_shell_event ev = { 0 };
	esp_err_t err;
	int ret = 0;

	(void)arg;
	captured_len = 0;
	fflush(stdout);
	console_out = stdout;
	capture_task = xTaskGetCurrentTaskHandle();
	stdout = &capture_stream;

	err = esp_console_run(job.cmd, &ret);

	stdout = console_out;
	capture_task = NULL;
	captured[captured_len] = '\0';

	memcpy(ev.output, captured, captured_len + 1);
	(void)tedge_shell_clean_output(ev.output);
	if (err == ESP_ERR_NOT_FOUND) {
		ev.rc = -ENOENT;
		snprintf(ev.output, sizeof(ev.output),
			 "the command is allowed but not registered in this "
			 "image");
	} else if (err != ESP_OK) {
		ev.rc = -EINVAL;
		snprintf(ev.output, sizeof(ev.output),
			 "the command could not run (%s)", esp_err_to_name(err));
	} else {
		ev.rc = ret;
		if (ret != 0 && ev.output[0] == '\0') {
			snprintf(ev.output, sizeof(ev.output),
				 "the command failed (%d)", ret);
		}
	}
	(void)tedge_port_msgq_put(&cmd_events, &ev);
	job.running = false;
}

/* The application may have set the console up already, to register its
 * commands; esp_console_run() needs it either way. */
static int console_ready(void)
{
	static bool ready;
	esp_console_config_t cfg = ESP_CONSOLE_CONFIG_DEFAULT();
	esp_err_t err;

	if (ready) {
		return 0;
	}
	err = esp_console_init(&cfg);
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		TEDGE_LOG_ERR("shell: the console did not start (%s)",
			      esp_err_to_name(err));
		return -EIO;
	}
	ready = true;
	return 0;
}

/* ------------------------------------------------------------------------ */
/* The request                                                               */
/* ------------------------------------------------------------------------ */

int tedge_shell_request(const char *line, char *reason, size_t rlen)
{
	char cmd[160];
	const char *why = "";

	/* 511,<device>,<command> */
	if (tedge_sr_field(line, 2, cmd, sizeof(cmd)) <= 0) {
		snprintf(reason, rlen, "malformed command");
		return -EINVAL;
	}
	if (job.running) {
		snprintf(reason, rlen, "a command is already running");
		return -EBUSY;
	}
	/* "help" answers with what this device runs: the console's own help
	 * would list every registered command, most of them refused here.
	 * Nothing is executed. */
	if (tedge_shell_is_help(cmd)) {
		struct tedge_shell_event ev = { 0 };

		(void)tedge_shell_help_text(TEDGE_SHELL_COMMAND_ALLOW_LIST,
					    ev.output, sizeof(ev.output));
		(void)tedge_port_msgq_put(&cmd_events, &ev);
		return 0;
	}
	if (!tedge_shell_command_allowed(TEDGE_SHELL_COMMAND_ALLOW_LIST, cmd,
					 &why)) {
		/* The command itself is not logged at INF: a refused command
		 * is someone else's text, and it lands in the cloud anyway. */
		TEDGE_LOG_WRN("shell: refused a command: %s", why);
		snprintf(reason, rlen, "%s; \"help\" lists what this device runs",
			 why);
		return -EACCES;
	}
	if (console_ready() != 0) {
		snprintf(reason, rlen, "the console did not start");
		return -EIO;
	}
	/* The last command has finished (job.running is false); its thread
	 * may still be a moment from returning. */
	(void)tedge_port_thread_join(&cmd_thread, 1000);

	snprintf(job.cmd, sizeof(job.cmd), "%s", cmd);
	job.started = tedge_port_uptime_ms();
	job.running = true;
	job.reported = false;
	TEDGE_LOG_INF("shell: running \"%s\"", job.cmd);
	if (tedge_port_thread_start(&cmd_thread, cmd_stack,
				    TEDGE_PORT_STACK_SIZEOF(cmd_stack),
				    run_thread, NULL, "tedge_cmd") != 0) {
		job.running = false;
		snprintf(reason, rlen, "the command thread did not start");
		return -EIO;
	}
	return 0;
}

int tedge_shell_poll_event(struct tedge_shell_event *ev)
{
	if (tedge_port_msgq_get(&cmd_events, ev) == 0) {
		return 0;
	}
	/* A console command cannot be interrupted, so a timeout reports the
	 * operation as failed and leaves the thread to finish. The next
	 * command is refused until it does. */
	if (job.running && !job.reported &&
	    tedge_port_uptime_ms() - job.started >
		    TEDGE_SHELL_COMMAND_TIMEOUT_S * 1000LL) {
		job.reported = true;
		memset(ev, 0, sizeof(*ev));
		ev->rc = -ETIMEDOUT;
		snprintf(ev->output, sizeof(ev->output),
			 "the command did not finish within %d s and is still "
			 "running",
			 TEDGE_SHELL_COMMAND_TIMEOUT_S);
		return 0;
	}
	return -ENOMSG;
}
