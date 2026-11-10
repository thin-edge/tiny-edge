/* SPDX-License-Identifier: Apache-2.0
 *
 * The POSIX port's configuration (tedge/posix.h) and the file and
 * interface helpers its other sources share.
 */

#include "tedge_posix_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static struct tedge_posix_config config;
static char state_dir[PATH_MAX];

int tedge_posix_configure(const struct tedge_posix_config *cfg)
{
	int ret;

	if (cfg == NULL || cfg->state_dir == NULL || cfg->state_dir[0] == '\0') {
		return -EINVAL;
	}
	if (snprintf(state_dir, sizeof(state_dir), "%s", cfg->state_dir) >=
	    (int)sizeof(state_dir)) {
		return -ENAMETOOLONG;
	}
	ret = tedge_posix_mkdirs(state_dir, 0700);
	if (ret != 0) {
		return ret;
	}
	config = *cfg;
	config.state_dir = state_dir;
	return 0;
}

const struct tedge_posix_config *tedge_posix_config(void)
{
	return &config;
}

int tedge_posix_path(char *buf, size_t len, const char *rel)
{
	if (snprintf(buf, len, "%s/%s", state_dir, rel) >= (int)len) {
		return -ENAMETOOLONG;
	}
	return 0;
}

int tedge_posix_mkdirs(const char *path, mode_t mode)
{
	char tmp[PATH_MAX];
	size_t len = strlen(path);

	if (len == 0 || len >= sizeof(tmp)) {
		return -ENAMETOOLONG;
	}
	memcpy(tmp, path, len + 1);
	for (char *p = tmp + 1; ; p++) {
		if (*p == '/' || *p == '\0') {
			char c = *p;

			*p = '\0';
			if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
				return -errno;
			}
			*p = c;
			if (c == '\0') {
				break;
			}
		}
	}
	return 0;
}

int tedge_posix_write_file(const char *path, const void *data, size_t len,
			   mode_t mode)
{
	char tmp[PATH_MAX];
	const uint8_t *p = data;
	int fd, ret = 0;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
		return -ENAMETOOLONG;
	}
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0) {
		return -errno;
	}
	while (len > 0) {
		ssize_t n = write(fd, p, len);

		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			ret = -errno;
			break;
		}
		p += n;
		len -= (size_t)n;
	}
	if (ret == 0 && fsync(fd) != 0) {
		ret = -errno;
	}
	if (close(fd) != 0 && ret == 0) {
		ret = -errno;
	}
	if (ret == 0 && rename(tmp, path) != 0) {
		ret = -errno;
	}
	if (ret != 0) {
		(void)unlink(tmp);
	}
	return ret;
}

ssize_t tedge_posix_read_file(const char *path, void *buf, size_t cap)
{
	struct stat st;
	ssize_t total = 0;
	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		return -errno;
	}
	if (fstat(fd, &st) != 0) {
		total = -errno;
		goto out;
	}
	while ((size_t)total < cap) {
		ssize_t n = read(fd, (uint8_t *)buf + total, cap - (size_t)total);

		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			total = -errno;
			goto out;
		}
		if (n == 0) {
			break;
		}
		total += n;
	}
	if ((size_t)st.st_size > (size_t)total) {
		total = (ssize_t)st.st_size;
	}
out:
	close(fd);
	return total;
}

int tedge_posix_interface(char name[IF_NAMESIZE])
{
	struct ifaddrs *list, *ifa;
	int ret = -ENODEV;

	if (config.interface != NULL) {
		snprintf(name, IF_NAMESIZE, "%s", config.interface);
		return 0;
	}
	if (getifaddrs(&list) != 0) {
		return -errno;
	}
	for (ifa = list; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET ||
		    (ifa->ifa_flags & IFF_LOOPBACK) || !(ifa->ifa_flags & IFF_UP)) {
			continue;
		}
		snprintf(name, IF_NAMESIZE, "%s", ifa->ifa_name);
		ret = 0;
		break;
	}
	freeifaddrs(list);
	return ret;
}

void tedge_posix_cloexec(int fd)
{
	int flags = fcntl(fd, F_GETFD);

	if (flags >= 0) {
		(void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
	}
}
