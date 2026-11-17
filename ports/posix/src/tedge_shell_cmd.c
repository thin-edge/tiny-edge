/* SPDX-License-Identifier: Apache-2.0
 *
 * Running a command the cloud asked for (c8y_Command) with popen(): the
 * system's shell runs it (/bin/sh on Linux and macOS, NSH on NuttX), and
 * what it prints on standard output is the operation's result.
 *
 * It runs on a thread of its own, so a command that takes a while does not
 * hold the connection up. What may run is decided by
 * core/src/tedge_shell_allow_list.c, which refuses everything until the
 * integrator lists commands in TEDGE_SHELL_COMMAND_ALLOW_LIST, and which
 * refuses anything that could chain a second command.
 */

#include <tedge/port/config.h>
#include <tedge/port/log.h>
#include <tedge/port/os.h>

#include "tedge_internal.h"

/* Builds that take every source (NuttX's Make build) compile it empty. */
#if defined(TEDGE_SHELL_COMMAND)

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

TEDGE_LOG_DECLARE();

TEDGE_PORT_STACK_DEFINE(cmd_stack, TEDGE_SHELL_COMMAND_STACK_SIZE);
static tedge_port_thread_t cmd_thread;
TEDGE_PORT_MSGQ_DEFINE(cmd_events, sizeof(struct tedge_shell_event), 2);

static struct {
	char cmd[160];
	int64_t started;
	volatile bool running;
	bool reported;
} job;

/* The event is too large for the thread's stack on a microcontroller. */
static struct tedge_shell_event result;

static void run_thread(void *arg)
{
	struct tedge_shell_event *ev = &result;
	size_t len = 0;
	char drain[64];
	FILE *out;
	int status;

	(void)arg;
	memset(ev, 0, sizeof(*ev));
	/* Standard error is not part of the result: "2>&1" would be a second
	 * redirection the allow-list refuses, so the port does not add it. */
	out = popen(job.cmd, "r");
	if (out == NULL) {
		ev->rc = -errno;
		snprintf(ev->output, sizeof(ev->output),
			 "the command could not start (%d)", errno);
		goto done;
	}
	/* Keep what fits; read the rest so the command does not block on a
	 * full pipe. */
	while (len < sizeof(ev->output) - 1) {
		size_t n = fread(ev->output + len, 1,
				 sizeof(ev->output) - 1 - len, out);

		if (n == 0) {
			break;
		}
		len += n;
	}
	while (fread(drain, 1, sizeof(drain), out) > 0) {
	}
	ev->output[len] = '\0';
	errno = 0;
	status = pclose(out);
	(void)tedge_shell_clean_output(ev->output);
	if (status == -1 && errno == ECHILD) {
		/* POSIX: the command ran, but its status is not available
		 * (NuttX without CONFIG_SCHED_CHILD_STATUS). What it printed is
		 * all there is to go by. */
		ev->rc = 0;
	} else if (status == -1) {
		ev->rc = errno != 0 ? -errno : -EIO;
	} else if (WIFEXITED(status)) {
		ev->rc = WEXITSTATUS(status);
	} else {
		ev->rc = -EINTR;
	}
	if (ev->rc == 127) {
		snprintf(ev->output, sizeof(ev->output),
			 "the command is allowed but not found on this device");
	} else if (ev->rc != 0) {
		/* The failure reason says it failed, then what it printed.
		 * Static: as large as the output, too large for the stack. */
		static char printed[sizeof(result.output)];

		size_t n, left;

		memcpy(printed, ev->output, sizeof(printed));
		n = (size_t)snprintf(ev->output, sizeof(ev->output),
				     "the command failed (%d)%s", ev->rc,
				     printed[0] != '\0' ? ": " : "");
		if (n < sizeof(ev->output) - 1) {
			/* What fits of the output, cut at the end. */
			left = sizeof(ev->output) - 1 - n;
			strncpy(ev->output + n, printed, left);
			ev->output[sizeof(ev->output) - 1] = '\0';
		}
	}
done:
	(void)tedge_port_msgq_put(&cmd_events, ev);
	job.running = false;
}

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
	/* "help" answers with what this device runs. Nothing is executed. */
	if (tedge_shell_is_help(cmd)) {
		struct tedge_shell_event *ev = &result;

		memset(ev, 0, sizeof(*ev));
		(void)tedge_shell_help_text(TEDGE_SHELL_COMMAND_ALLOW_LIST,
					    ev->output, sizeof(ev->output));
		(void)tedge_port_msgq_put(&cmd_events, ev);
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
	/* popen() gives no process to stop, so a timeout reports the operation
	 * as failed and leaves the command to finish. The next command is
	 * refused until it does. */
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

#endif /* TEDGE_SHELL_COMMAND */
