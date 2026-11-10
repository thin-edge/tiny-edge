/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief What the POSIX port needs from the application.
 *
 * On an MCU the client finds what it needs in the image: a settings
 * partition, the one network interface, the firmware's version. A POSIX
 * process has none of that fixed, so the application names it here, once,
 * before tedge_init():
 *
 * @code
 * struct tedge_posix_config cfg = {
 *	.state_dir = "/var/lib/tedge-posix",
 *	.argv = argv,
 * };
 * if (tedge_posix_configure(&cfg) == 0 && tedge_init(NULL, &hooks) == 0) {
 *	tedge_start();
 * }
 * @endcode
 */

#ifndef TEDGE_POSIX_H_
#define TEDGE_POSIX_H_

#ifdef __cplusplus
extern "C" {
#endif

/** The exit status of a restart when the port has no command line to start
 *  again with: EX_TEMPFAIL, which a supervisor (systemd's Restart=,
 *  launchd's KeepAlive) restarts. */
#define TEDGE_POSIX_EXIT_RESTART 75

struct tedge_posix_config {
	/** Where the client keeps what it must remember across a restart:
	 *  its settings and the device key. Created, readable by the owner
	 *  only, when missing. Required. */
	const char *state_dir;
	/** The network interface the client uses ("eth0", "en0"): its MAC
	 *  makes the default device id, and remote access treats its subnet
	 *  as the LAN. NULL picks the first interface that is up and has an
	 *  IPv4 address, other than the loopback. */
	const char *interface;
	/** The firmware's name and version, as Cumulocity shows them. NULL
	 *  gives the program's name and "0.0.0". */
	const char *firmware_name;
	const char *firmware_version;
	/** The command line the process was started with (main's argv): a
	 *  restart starts it again with execv(). NULL makes a restart end the
	 *  process with TEDGE_POSIX_EXIT_RESTART instead. */
	char *const *argv;
};

/**
 * Configure the port. Call once, before tedge_init(). The strings and argv
 * must stay valid while the client runs, apart from state_dir, which is
 * copied.
 *
 * @return 0, or a negative errno: -EINVAL without a state_dir, or the error
 *         creating it.
 */
int tedge_posix_configure(const struct tedge_posix_config *config);

#ifdef __cplusplus
}
#endif

#endif /* TEDGE_POSIX_H_ */
