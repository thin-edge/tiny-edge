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
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <ifaddrs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>

#if defined(TEDGE_POSIX_MAC_IOCTL)
#include <net/if.h>
#include <sys/ioctl.h>
#elif defined(__linux__)
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

#if defined(TEDGE_POSIX_MAC_IOCTL)

/* The interface's MAC from SIOCGIFHWADDR, for systems whose getifaddrs()
 * lists no link-layer entries (NuttX). Linux has it too. */
int tedge_port_mac_address(uint8_t mac[6])
{
	char name[IF_NAMESIZE];
	struct ifreq ifr;
	int fd;
	int ret = tedge_posix_interface(name);

	if (ret != 0) {
		return ret;
	}
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return -errno;
	}
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, name, sizeof(ifr.ifr_name) - 1);
	ret = ioctl(fd, SIOCGIFHWADDR, (unsigned long)(uintptr_t)&ifr) == 0
		      ? 0 : -errno;
	(void)close(fd);
	if (ret == 0) {
		memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
	}
	return ret;
}

#else /* MAC from getifaddrs() */

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

#endif /* TEDGE_POSIX_MAC_IOCTL */

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
	const struct tedge_posix_config *cfg = tedge_posix_config();
	char *const *argv = cfg->argv;

	(void)fflush(NULL);
	if (cfg->restart != NULL) {
		tedge_port_log_flush();
		cfg->restart();
		TEDGE_LOG_ERR("restart: the application's restart returned");
		tedge_port_log_flush();
	}
#if defined(TEDGE_POSIX_NO_EXEC)
	/* No exec() on this system (TEDGE_POSIX_NO_EXEC): only the
	 * application's restart can start the client again. */
	(void)argv;
#else
	if (argv != NULL && argv[0] != NULL) {
		(void)execv(argv[0], argv);
		/* argv[0] without a slash is found on the PATH, as the shell
		 * found it. */
		(void)execvp(argv[0], argv);
		TEDGE_LOG_ERR("restart: cannot run %s again (%d); exiting",
			      argv[0], errno);
		tedge_port_log_flush();
	}
#endif
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

/* Anything before 2025-01-01 means "not set", and so does anything after
 * 2100: some boards keep counting through a reset from a value that is no
 * date at all. */
#define CLOCK_FLOOR 1735689600
#define CLOCK_CEILING 4102444800LL

static bool clock_plausible(time_t now)
{
	return now > CLOCK_FLOOR && (int64_t)now < CLOCK_CEILING;
}

bool tedge_port_time_is_valid(void)
{
	return clock_plausible(time(NULL));
}

#if defined(TEDGE_SNTP)
/* One SNTP request (RFC 4330) to TEDGE_SNTP_SERVER, and the clock set from
 * the answer, accurate to the round trip, which is enough for TLS. */
int tedge_port_time_sync(void)
{
	struct addrinfo hints = { .ai_family = AF_INET,
				  .ai_socktype = SOCK_DGRAM };
	struct addrinfo *ai = NULL;
	uint8_t pkt[48] = { 0x23 }; /* LI 0, version 4, mode 3 (client) */
	struct timeval tv = { .tv_sec = 3 };
	struct timespec ts = { 0 };
	uint32_t secs;
	int fd, ret = -ETIMEDOUT;

	if (tedge_port_time_is_valid()) {
		return 0;
	}
	if (getaddrinfo(TEDGE_SNTP_SERVER, "123", &hints, &ai) != 0 ||
	    ai == NULL) {
		TEDGE_LOG_WRN("SNTP: cannot resolve %s", TEDGE_SNTP_SERVER);
		return -EHOSTUNREACH;
	}
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		ret = -errno;
	} else {
		(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		if (sendto(fd, pkt, sizeof(pkt), 0, ai->ai_addr,
			   ai->ai_addrlen) == sizeof(pkt) &&
		    recv(fd, pkt, sizeof(pkt), 0) == sizeof(pkt) &&
		    (pkt[0] & 0x07) == 4 && pkt[1] != 0) {
			/* The transmit timestamp, in seconds since 1900. */
			secs = ((uint32_t)pkt[40] << 24) |
			       ((uint32_t)pkt[41] << 16) |
			       ((uint32_t)pkt[42] << 8) | pkt[43];
			ts.tv_sec = (time_t)(secs - 2208988800u);
			ret = (clock_plausible(ts.tv_sec) &&
			       clock_settime(CLOCK_REALTIME, &ts) == 0)
				      ? 0 : -EIO;
		}
		(void)close(fd);
	}
	freeaddrinfo(ai);
	if (ret == 0) {
		TEDGE_LOG_INF("clock set with SNTP from %s", TEDGE_SNTP_SERVER);
	} else {
		TEDGE_LOG_WRN("SNTP: no answer from %s (%d)", TEDGE_SNTP_SERVER,
			      ret);
	}
	return ret;
}
#else
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
#endif

int tedge_port_time_now(time_t *secs)
{
	time_t now = time(NULL);

	if (!clock_plausible(now)) {
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
