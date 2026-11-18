/* SPDX-License-Identifier: Apache-2.0
 *
 * The device agent on NuttX: the thin-edge.io client with no protocol
 * server, reporting the device's own vitals as the "device" measurement.
 * As on the other ports, the application owns the network: it joins the
 * Wi-Fi network stored on the device (tedge_nuttx_wifi_join_stored()), and
 * the client sets the clock with SNTP before it connects
 * (CONFIG_TEDGE_SNTP). With CONFIG_TEDGE_PROVISIONING, tedge-onboard sets
 * it up over USB.
 *
 *   tiny_edge [-c tenant-host] [-o one-time-password]
 *
 * Without a certificate the client prints a registration URL, or enrolls
 * with the one-time password from -o (a device registered beforehand).
 */

#include <nuttx/config.h>

#include <tedge/nuttx.h>
#include <tedge/posix.h>
#include <tedge/tedge.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/boardctl.h>

#include "netutils/netlib.h"

#define IFNAME CONFIG_TINY_EDGE_INTERFACE

/* --- The network ------------------------------------------------------- */

/* Joins the stored network, trying until it works: after a software
 * reset the ESP chips' Wi-Fi driver can fail to join for a while. Set-up
 * over USB may join a network meanwhile, which ends the wait too. */
static int join_network(void)
{
	struct in_addr addr;
	int ret = tedge_nuttx_wifi_join_stored();
	unsigned int wait = 10;

	while (ret != 0 && ret != -ENOENT) {
		printf("agent: Wi-Fi: not joined (%d); trying again in %u s\n",
		       ret, wait);
		sleep(wait);
		if (netlib_get_ipv4addr(CONFIG_TINY_EDGE_INTERFACE, &addr) == 0 &&
		    addr.s_addr != 0) {
			return 0;
		}
		ret = tedge_nuttx_wifi_join_stored();
		wait = wait < 60 ? wait * 2 : 60;
	}
	return ret;
}

/* --- The client -------------------------------------------------------- */

static void on_state(enum tedge_state state, void *user_data)
{
	(void)user_data;
	if (state == TEDGE_STATE_AWAITING_REGISTRATION) {
		char url[220];

		/* The URL carries a one-time password: it is for the person
		 * at the console. */
		if (tedge_registration_url(url, sizeof(url)) > 0) {
			printf("register this device: %s\n", url);
		}
	}
}

/* A restart from the cloud resets the board, as on the other ports. */
static void board_reset(void)
{
	(void)boardctl(BOARDIOC_RESET, 0);
}

static const struct tedge_hooks hooks = {
	.on_state = on_state,
};

#if defined(CONFIG_STACK_COLORATION) && defined(CONFIG_TEDGE_LOG_UPLOAD)
/* --- The "stacks" log --------------------------------------------------- */

/* Each thread's stack size and the most of it ever used, from procfs, as a
 * log type the cloud can request at any time: a long run can then check
 * the headroom without a program reading the USB port. The client may call
 * the reader twice for one request (to measure, then to send), so one
 * snapshot serves both. */
static char stacks[2048];
static size_t stacks_len;
static struct timespec stacks_at;

/* The value after @p key in a procfs file, or "" when it has none. */
static void proc_field(const char *path, const char *key, char *out,
		       size_t len)
{
	char line[64];
	FILE *f = fopen(path, "r");

	out[0] = '\0';
	if (f == NULL) {
		return;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, key, strlen(key)) == 0) {
			const char *v = line + strlen(key);

			v += strspn(v, " \t");
			snprintf(out, len, "%.*s", (int)strcspn(v, "\n"), v);
			break;
		}
	}
	fclose(f);
}

static void snapshot_stacks(void)
{
	DIR *dir = opendir("/proc");
	struct dirent *de;
	size_t n;

	n = snprintf(stacks, sizeof(stacks), "%5s %-20s %6s %6s %4s\n", "PID",
		     "NAME", "SIZE", "USED", "USE");
	while (dir != NULL && (de = readdir(dir)) != NULL &&
	       n < sizeof(stacks)) {
		char path[48], name[24], size[16], used[16];
		long sz, us;

		if (de->d_name[0] < '0' || de->d_name[0] > '9') {
			continue;
		}
		snprintf(path, sizeof(path), "/proc/%s/status", de->d_name);
		proc_field(path, "Name:", name, sizeof(name));
		snprintf(path, sizeof(path), "/proc/%s/stack", de->d_name);
		proc_field(path, "StackSize:", size, sizeof(size));
		proc_field(path, "StackUsed:", used, sizeof(used));
		sz = strtol(size, NULL, 10);
		us = strtol(used, NULL, 10);
		n += snprintf(stacks + n, sizeof(stacks) - n,
			      "%5s %-20.20s %6ld %6ld %3ld%%\n", de->d_name,
			      name, sz, us, sz > 0 ? us * 100 / sz : 0);
	}
	if (dir != NULL) {
		closedir(dir);
	}
	stacks_len = n < sizeof(stacks) ? n : sizeof(stacks) - 1;
}

static int read_stacks(const struct tedge_log_request *req,
		       tedge_write_fn write, void *ctx, void *user_data)
{
	struct timespec now;

	(void)req;
	(void)user_data;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	if (stacks_len == 0 || now.tv_sec - stacks_at.tv_sec > 5) {
		snapshot_stacks();
		stacks_at = now;
	}
	return write(ctx, stacks, stacks_len);
}
#endif

/* The device's own vitals, as the other ports' agents send them. */
static void publish_vitals(void)
{
	struct mallinfo mi = mallinfo();
	struct timespec now;
	struct tedge_measurement_value values[2];

	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	values[0] = (struct tedge_measurement_value){
		.series = "uptime", .unit = "s", .value = (double)now.tv_sec,
	};
	values[1] = (struct tedge_measurement_value){
		.series = "heap_free", .unit = "B", .value = (double)mi.fordblks,
	};
	(void)tedge_publish_measurement("device", values, 2, 0);
}

int main(int argc, char *argv[])
{
	const char *host = NULL;
	const char *otp = NULL;
	struct tedge_posix_config cfg = {
		.state_dir = CONFIG_TINY_EDGE_STATE_DIR,
		.interface = IFNAME,
		.firmware_name = CONFIG_TINY_EDGE_FIRMWARE_NAME,
		.firmware_version = CONFIG_TINY_EDGE_FIRMWARE_VERSION[0] != '\0'
					    ? CONFIG_TINY_EDGE_FIRMWARE_VERSION
					    : TEDGE_VERSION_STRING,
		.restart = board_reset,
	};
	int opt;

	while ((opt = getopt(argc, argv, "c:o:")) != -1) {
		switch (opt) {
		case 'c':
			host = optarg;
			break;
		case 'o':
			otp = optarg;
			break;
		default:
			fprintf(stderr, "usage: %s [-c tenant-host] "
				"[-o one-time-password]\n", argv[0]);
			return EXIT_FAILURE;
		}
	}

	/* The console is the board's USB serial port. With no program on the
	 * computer reading it, its buffer fills and a blocking write waits
	 * for ever: the first log line after that stopped the client. Log
	 * lines are dropped instead. Set-up over USB writes through its own,
	 * blocking descriptor, and only while tedge-onboard is reading. */
	(void)fcntl(STDOUT_FILENO, F_SETFL,
		    fcntl(STDOUT_FILENO, F_GETFL) | O_NONBLOCK);
	(void)fcntl(STDERR_FILENO, F_SETFL,
		    fcntl(STDERR_FILENO, F_GETFL) | O_NONBLOCK);

	printf("agent: %s %s (tiny-edge %s) on NuttX\n", cfg.firmware_name,
	       cfg.firmware_version, tedge_version());
	/* NuttX's uname() names the architecture ("xtensa", "risc-v"), not
	 * the chip, so the chip is given here for c8y_Hardware. */
	static const struct tedge_identity id = {
		.hardware_model = CONFIG_ARCH_CHIP,
	};

	if (tedge_posix_configure(&cfg) != 0 || tedge_init(&id, &hooks) != 0) {
		printf("agent: the client did not start\n");
		return EXIT_FAILURE;
	}
#if defined(CONFIG_STACK_COLORATION) && defined(CONFIG_TEDGE_LOG_UPLOAD)
	(void)tedge_register_log_type("stacks", read_stacks, NULL);
#endif
	if (host != NULL) {
		(void)tedge_set_c8y_url(host);
	}
	if (otp != NULL) {
		(void)tedge_set_enroll_otp(otp);
	}
#if defined(CONFIG_TEDGE_PROVISIONING)
	/* Set-up over USB: tedge-onboard gives the device its network, the
	 * tenant, and a one-time password. */
	if (tedge_provisioning_nuttx_init(NULL) != 0) {
		printf("agent: set-up over USB did not start\n");
	}
#endif

	/* After set-up over USB has started, so tedge-onboard finds the board
	 * at once: it resets the board when it opens the port, and joining
	 * takes a while. */
	opt = join_network();
	if (opt == -ENOENT) {
		printf("agent: no Wi-Fi network yet: set the device up with "
		       "tedge-onboard\n");
	}

	/* Set-up may have started the client already. */
	opt = tedge_start();
	if (opt != 0 && opt != -EALREADY) {
		printf("agent: the client did not start (%d)\n", opt);
		return EXIT_FAILURE;
	}

	for (;;) {
		sleep(CONFIG_TINY_EDGE_MEASUREMENT_INTERVAL_S);
		publish_vitals();
	}
	return EXIT_SUCCESS;
}
