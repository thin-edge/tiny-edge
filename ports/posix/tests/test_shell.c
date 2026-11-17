/* SPDX-License-Identifier: Apache-2.0
 *
 * The shell command (c8y_Command) through popen(): an allowed command runs
 * and its output is the result, a command the allow-list does not name is
 * refused without running, and "help" lists the allow-list. Build with
 * TEDGE_SHELL_COMMAND_ALLOW_LIST containing "echo" to run the first part.
 */

#include <tedge/port/config.h>
#include <tedge/port/os.h>

#include "tedge_internal.h"

#include <errno.h>
#include <stdio.h>
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

static struct tedge_shell_event ev;

/* The result of the command started last, waiting up to 5 s. */
static int wait_event(void)
{
	for (int i = 0; i < 500; i++) {
		if (tedge_shell_poll_event(&ev) == 0) {
			return 0;
		}
		tedge_port_sleep_ms(10);
	}
	return -ETIMEDOUT;
}

int main(void)
{
	char reason[128] = "";

	if (strstr(TEDGE_SHELL_COMMAND_ALLOW_LIST, "echo") != NULL) {
		CHECK(tedge_shell_request("511,dev,echo hello from the shell",
					  reason, sizeof(reason)) == 0);
		CHECK(wait_event() == 0);
		CHECK(ev.rc == 0);
		CHECK(strstr(ev.output, "hello from the shell") != NULL);
	} else {
		printf("skipped: echo is not in the allow-list\n");
	}

	/* A command that fails: the reason says so, not only what it printed. */
	if (strstr(TEDGE_SHELL_COMMAND_ALLOW_LIST, "uname") != NULL) {
		CHECK(tedge_shell_request("511,dev,uname --no-such-option",
					  reason, sizeof(reason)) == 0);
		CHECK(wait_event() == 0);
		CHECK(ev.rc != 0);
		CHECK(strncmp(ev.output, "the command failed", 18) == 0);
	}

	/* Not on the list: refused, nothing runs. */
	CHECK(tedge_shell_request("511,dev,touch /tmp/tedge-shell-test", reason,
				  sizeof(reason)) == -EACCES);
	CHECK(strstr(reason, "help") != NULL);

	/* A second command chained onto an allowed one: refused too. */
	CHECK(tedge_shell_request("511,dev,echo a; touch /tmp/x", reason,
				  sizeof(reason)) == -EACCES);

	/* "help" lists the allow-list without running anything. */
	CHECK(tedge_shell_request("511,dev,help", reason, sizeof(reason)) == 0);
	CHECK(wait_event() == 0);
	CHECK(ev.rc == 0);

	if (failures == 0) {
		printf("shell: all checks passed\n");
	}
	return failures == 0 ? 0 : 1;
}
