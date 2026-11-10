/* SPDX-License-Identifier: Apache-2.0
 *
 * The smallest program that hosts the thin-edge.io client on POSIX: tell
 * the port where to keep its state, start the client, and wait. The client
 * connects, enrolls and reports by itself.
 *
 *   tedge-minimal [-s state-dir] [-c tenant-host] [-i interface] [-d id]
 *                 [-o one-time-password]
 *
 * With certificate enrollment (the default), the first run prints a URL:
 * open it to register the device in Cumulocity. The device key and
 * certificate are kept in the state directory, so later runs connect at
 * once. A device registered beforehand (c8y deviceregistration
 * register-ca, or a provisioning server) takes its one-time password with
 * -o instead, and shows no URL.
 */

#include <tedge/posix.h>
#include <tedge/tedge.h>

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void on_state(enum tedge_state state, void *user_data)
{
	(void)user_data;

	if (state == TEDGE_STATE_AWAITING_REGISTRATION) {
		char url[220];

		/* The URL carries a one-time password: it is for the operator
		 * at the console, not for a log that others read. */
		if (tedge_registration_url(url, sizeof(url)) > 0) {
			printf("register this device: %s\n", url);
			fflush(stdout);
		}
	}
}

static const struct tedge_hooks hooks = {
	.on_state = on_state,
};

/* The command line a restart runs again: this one without -o. Setting a
 * one-time password re-onboards the device (tedge_set_enroll_otp()), so
 * passing it again on every start would discard the certificate each
 * time. Copied before getopt(), which reorders argv on glibc. */
static char **restart_argv(int argc, char **argv)
{
	char **out = calloc((size_t)argc + 1, sizeof(*out));
	int n = 0;

	if (out == NULL) {
		return NULL;
	}
	for (int i = 0; i < argc; i++) {
		if (i > 0 && strcmp(argv[i], "-o") == 0) {
			i++; /* and its value */
			continue;
		}
		if (i > 0 && strncmp(argv[i], "-o", 2) == 0) {
			continue; /* -o<password> */
		}
		out[n++] = argv[i];
	}
	return out;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-s state-dir] [-c tenant-host] [-i interface] "
		"[-d device-id] [-o otp]\n"
		"  -s  where the client keeps its key and settings "
		"(default: .tedge-posix)\n"
		"  -c  the Cumulocity tenant, when the build does not name it\n"
		"  -i  the network interface to use (default: the first that "
		"is up)\n"
		"  -d  the device's external ID (default: made from the "
		"interface's MAC)\n"
		"  -o  a one-time password the device was registered with\n",
		prog);
}

int main(int argc, char **argv)
{
	struct tedge_posix_config cfg = {
		.state_dir = ".tedge-posix",
		.firmware_name = "tedge-minimal",
		.firmware_version = tedge_version(),
	};
	struct tedge_identity id = { 0 };
	const char *c8y_url = NULL;
	const char *otp = NULL;
	sigset_t stop;
	int opt, sig, ret;

	cfg.argv = restart_argv(argc, argv);
	while ((opt = getopt(argc, argv, "s:c:i:d:o:h")) != -1) {
		switch (opt) {
		case 's':
			cfg.state_dir = optarg;
			break;
		case 'c':
			c8y_url = optarg;
			break;
		case 'i':
			cfg.interface = optarg;
			break;
		case 'd':
			id.external_id = optarg;
			break;
		case 'o':
			otp = optarg;
			break;
		default:
			usage(argv[0]);
			return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	/* The client's threads must not take the signals main() waits for. */
	sigemptyset(&stop);
	sigaddset(&stop, SIGINT);
	sigaddset(&stop, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &stop, NULL);

	fprintf(stderr, "tedge-minimal %s\n", tedge_version());
	ret = tedge_posix_configure(&cfg);
	if (ret != 0) {
		fprintf(stderr, "state directory %s: %s\n", cfg.state_dir,
			strerror(-ret));
		return EXIT_FAILURE;
	}
	ret = tedge_init(id.external_id != NULL ? &id : NULL, &hooks);
	if (ret == 0 && c8y_url != NULL) {
		ret = tedge_set_c8y_url(c8y_url);
	}
	if (ret == 0 && otp != NULL) {
		ret = tedge_set_enroll_otp(otp);
	}
	if (ret == 0) {
		ret = tedge_start();
	}
	if (ret != 0) {
		fprintf(stderr, "the client did not start: %s\n", strerror(-ret));
		return EXIT_FAILURE;
	}

	(void)sigwait(&stop, &sig);
	fprintf(stderr, "stopping\n");
	(void)tedge_stop();
	return EXIT_SUCCESS;
}
