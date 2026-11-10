/* SPDX-License-Identifier: Apache-2.0
 *
 * The client stopped and started again, as a provisioning library does to
 * hand it new onboarding data: the stop must end the client thread
 * promptly even while it waits between enrollment attempts, and the client
 * must start again without declaring anything twice.
 *
 * The tenant is 127.0.0.1, which refuses the connection, so the client
 * sits in its enrollment wait (10 s and more) when it is stopped.
 */

#include <tedge/port/os.h>
#include <tedge/posix.h>
#include <tedge/tedge.h>

#include <errno.h>
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

static enum tedge_state last_state;

static void on_state(enum tedge_state state, void *user_data)
{
	(void)user_data;
	last_state = state;
}

static const struct tedge_hooks hooks = { .on_state = on_state };

/* Wait up to @p ms for the client to wait for registration. */
static bool awaiting(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 50) {
		if (last_state == TEDGE_STATE_AWAITING_REGISTRATION) {
			return true;
		}
		tedge_port_sleep_ms(50);
	}
	return false;
}

int main(void)
{
	char dir[] = "/tmp/tedge-lifecycle-test-XXXXXX";
	struct tedge_posix_config cfg = { 0 };
	struct tedge_identity id = { .external_id = "tedge-lifecycle-test" };
	int64_t t0;

	if (mkdtemp(dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	cfg.state_dir = dir;
	CHECK(tedge_posix_configure(&cfg) == 0);
	CHECK(tedge_init(&id, &hooks) == 0);
	CHECK(tedge_set_c8y_url("127.0.0.1") == 0);
	CHECK(tedge_stop() == -EALREADY);
	CHECK(tedge_start() == 0);
	CHECK(tedge_start() == -EALREADY);

	/* Enrolling: the first attempt is refused, then it waits. */
	CHECK(awaiting(5000));
	tedge_port_sleep_ms(500);
	t0 = tedge_port_uptime_ms();
	CHECK(tedge_stop() == 0);
	CHECK(tedge_port_uptime_ms() - t0 < 2000); /* not the 10 s wait */

	/* New onboarding data, as a provisioning library hands it over. */
	CHECK(tedge_set_c8y_url("127.0.0.1") == 0);
	CHECK(tedge_set_enroll_otp("Abcdefghijklmnopqrstuvwx") == 0);
	last_state = TEDGE_STATE_STOPPED;
	CHECK(tedge_start() == 0);
	CHECK(awaiting(5000));
	t0 = tedge_port_uptime_ms();
	CHECK(tedge_stop() == 0);
	CHECK(tedge_port_uptime_ms() - t0 < 2000);

	{
		char rm[80];

		snprintf(rm, sizeof(rm), "rm -rf %s", dir);
		(void)!system(rm);
	}
	if (failures == 0) {
		printf("stop and start: all checks passed\n");
	}
	return failures == 0 ? 0 : 1;
}
