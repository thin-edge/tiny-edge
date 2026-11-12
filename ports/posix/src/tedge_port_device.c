/* SPDX-License-Identifier: Apache-2.0
 *
 * The POSIX side of <tedge/port/device.h> and <tedge/port/time.h>: the
 * interface's MAC address, the firmware's name and version from the
 * application, a restart that starts the process again, and the system
 * clock.
 */

#include <tedge/port/device.h>
#include <tedge/port/log.h>
#include <tedge/port/time.h>

#include "tedge_posix_internal.h"

#include <ctype.h>
#include <errno.h>
#include <ifaddrs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>

#if defined(__linux__)
#include <netpacket/packet.h>
#else
#include <net/if_dl.h>
#endif

TEDGE_LOG_DECLARE();

/* --- The device ---------------------------------------------------------- */

/* A process has no reset cause of its own; the system's is not the
 * client's to report. */
int tedge_port_reset_cause(uint32_t *cause)
{
	*cause = 0;
	return -ENOTSUP;
}

static bool link_mac(const struct ifaddrs *ifa, uint8_t mac[6])
{
#if defined(__linux__)
	const struct sockaddr_ll *ll = (const void *)ifa->ifa_addr;

	if (ifa->ifa_addr->sa_family != AF_PACKET || ll->sll_halen != 6) {
		return false;
	}
	memcpy(mac, ll->sll_addr, 6);
#else
	const struct sockaddr_dl *dl = (const void *)ifa->ifa_addr;

	if (ifa->ifa_addr->sa_family != AF_LINK || dl->sdl_alen != 6) {
		return false;
	}
	memcpy(mac, LLADDR(dl), 6);
#endif
	return true;
}

int tedge_port_mac_address(uint8_t mac[6])
{
	char name[IF_NAMESIZE];
	struct ifaddrs *list, *ifa;
	int ret = tedge_posix_interface(name);

	if (ret != 0) {
		return ret;
	}
	if (getifaddrs(&list) != 0) {
		return -errno;
	}
	ret = -ENODEV;
	for (ifa = list; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr != NULL && strcmp(ifa->ifa_name, name) == 0 &&
		    link_mac(ifa, mac)) {
			ret = 0;
			break;
		}
	}
	freeifaddrs(list);
	return ret;
}

const char *tedge_port_firmware_name(void)
{
	const struct tedge_posix_config *cfg = tedge_posix_config();
	const char *slash;

	if (cfg->firmware_name != NULL) {
		return cfg->firmware_name;
	}
	if (cfg->argv != NULL && cfg->argv[0] != NULL) {
		slash = strrchr(cfg->argv[0], '/');
		return (slash != NULL) ? slash + 1 : cfg->argv[0];
	}
	return "tedge-posix";
}

const char *tedge_port_firmware_version(void)
{
	const char *version = tedge_posix_config()->firmware_version;

	return (version != NULL) ? version : "0.0.0";
}

/* The process starts again from the top, as a device boots: the same
 * program and arguments, a fresh client. Every descriptor the port opened
 * is close-on-exec. Without the command line, the supervisor does it. */
_Noreturn void tedge_port_reset(void)
{
	char *const *argv = tedge_posix_config()->argv;

	(void)fflush(NULL);
	if (argv != NULL && argv[0] != NULL) {
		(void)execv(argv[0], argv);
		/* argv[0] without a slash is found on the PATH, as the shell
		 * found it. */
		(void)execvp(argv[0], argv);
		TEDGE_LOG_ERR("restart: cannot run %s again (%d); exiting",
			      argv[0], errno);
		tedge_port_log_flush();
	}
	_exit(TEDGE_POSIX_EXIT_RESTART);
}

/* Crash dumps are the system's on POSIX (core files, systemd-coredump). */
void tedge_port_coredump_init(void)
{
}

void tedge_port_coredump_taken(void)
{
}

/* --- Time ---------------------------------------------------------------- */

/* Anything before 2025-01-01 means "not set". */
#define CLOCK_FLOOR 1735689600

bool tedge_port_time_is_valid(void)
{
	return time(NULL) > CLOCK_FLOOR;
}

/* The system keeps the clock (NTP, chrony, systemd-timesyncd); a process
 * has no business setting it. */
int tedge_port_time_sync(void)
{
	if (tedge_port_time_is_valid()) {
		return 0;
	}
	TEDGE_LOG_WRN("the system clock is not set: the client cannot verify "
		      "the server until it is");
	return -EAGAIN;
}

int tedge_port_time_now(time_t *secs)
{
	time_t now = time(NULL);

	if (now <= CLOCK_FLOOR) {
		return -EAGAIN;
	}
	*secs = now;
	return 0;
}

/* The host's kernel and machine, from uname(): "linux" or "darwin", its
 * release, and the machine type ("x86_64", "arm64"). */
void tedge_port_platform(struct tedge_port_platform *out)
{
	static struct utsname host;
	static char os[sizeof(host.sysname)];

	memset(out, 0, sizeof(*out));
	if (uname(&host) != 0) {
		return;
	}
	for (size_t i = 0; i < sizeof(os) - 1 && host.sysname[i] != '\0'; i++) {
		os[i] = (char)tolower((unsigned char)host.sysname[i]);
	}
	out->os = os;
	out->os_version = host.release;
	out->model = host.machine;
}
