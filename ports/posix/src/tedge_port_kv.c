/* SPDX-License-Identifier: Apache-2.0
 *
 * The POSIX side of <tedge/port/kv.h>: one file per key under the state
 * directory, the key's levels as directories,
 *
 *   tedge/param/modbus/unit_id  ->  <state_dir>/kv/tedge/param/modbus/unit_id.v
 *
 * The ".v" keeps a value apart from the directory of the keys below it, as
 * "tedge/firmware" and "tedge/firmware/..." may both exist. A value is
 * replaced all at once (tedge_posix_write_file()), so a crash leaves either
 * the old value or the new one.
 */

#include <tedge/port/kv.h>

#include "tedge_posix_internal.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SUFFIX ".v"

/* A key is levels of [A-Za-z0-9_.-] between single slashes, none "." or
 * "..", so it can only name a file below the store. */
static bool key_valid(const char *key)
{
	const char *level = key;

	if (*key == '\0') {
		return false;
	}
	for (const char *p = key; ; p++) {
		if (*p == '/' || *p == '\0') {
			size_t len = (size_t)(p - level);

			if (len == 0 || (len == 1 && level[0] == '.') ||
			    (len == 2 && level[0] == '.' && level[1] == '.')) {
				return false;
			}
			if (*p == '\0') {
				return true;
			}
			level = p + 1;
			continue;
		}
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' ||
		      *p == '.')) {
			return false;
		}
	}
}

static int key_path(char *buf, size_t len, const char *key, const char *suffix)
{
	char rel[PATH_MAX];

	if (!key_valid(key)) {
		return -EINVAL;
	}
	if (snprintf(rel, sizeof(rel), "kv/%s%s", key, suffix) >= (int)sizeof(rel)) {
		return -ENAMETOOLONG;
	}
	return tedge_posix_path(buf, len, rel);
}

int tedge_port_kv_init(void)
{
	char dir[PATH_MAX];
	int ret = tedge_posix_path(dir, sizeof(dir), "kv");

	return (ret == 0) ? tedge_posix_mkdirs(dir, 0700) : ret;
}

int tedge_port_kv_set(const char *key, const void *value, size_t len)
{
	char path[PATH_MAX];
	char *slash;
	int ret = key_path(path, sizeof(path), key, SUFFIX);

	if (ret != 0) {
		return ret;
	}
	slash = strrchr(path, '/');
	*slash = '\0';
	ret = tedge_posix_mkdirs(path, 0700);
	*slash = '/';
	return (ret == 0) ? tedge_posix_write_file(path, value, len, 0600) : ret;
}

int tedge_port_kv_get(const char *key, void *buf, size_t cap)
{
	char path[PATH_MAX];
	ssize_t n;
	int ret = key_path(path, sizeof(path), key, SUFFIX);

	if (ret != 0) {
		return ret;
	}
	n = tedge_posix_read_file(path, buf, cap);
	if (n < 0) {
		return (int)n;
	}
	return ((size_t)n > cap) ? -ENOSPC : (int)n;
}

int tedge_port_kv_delete(const char *key)
{
	char path[PATH_MAX];
	int ret = key_path(path, sizeof(path), key, SUFFIX);

	if (ret != 0) {
		return ret;
	}
	return (unlink(path) == 0 || errno == ENOENT) ? 0 : -errno;
}

/* Calls @p cb for every value file under @p dir; @p sub is the subkey that
 * @p dir stands for so far ("" at the top), in a buffer the walk extends. */
static int walk(char *dir, size_t dir_cap, char *sub, size_t sub_cap,
		void (*cb)(const char *subkey, void *ctx), void *ctx)
{
	size_t dir_len = strlen(dir), sub_len = strlen(sub);
	struct dirent *e;
	DIR *d = opendir(dir);
	int ret = 0;

	if (d == NULL) {
		return (errno == ENOENT) ? 0 : -errno;
	}
	while (ret == 0 && (e = readdir(d)) != NULL) {
		size_t name_len = strlen(e->d_name);
		struct stat st;

		if (e->d_name[0] == '.') {
			continue; /* ".", "..", and no key level starts with "." */
		}
		if (dir_len + 1 + name_len >= dir_cap ||
		    sub_len + 1 + name_len >= sub_cap) {
			ret = -ENAMETOOLONG;
			break;
		}
		snprintf(dir + dir_len, dir_cap - dir_len, "/%s", e->d_name);
		snprintf(sub + sub_len, sub_cap - sub_len, "%s%s",
			 sub_len > 0 ? "/" : "", e->d_name);
		if (stat(dir, &st) == 0) {
			if (S_ISDIR(st.st_mode)) {
				ret = walk(dir, dir_cap, sub, sub_cap, cb, ctx);
			} else if (S_ISREG(st.st_mode) && name_len > 2 &&
				   strcmp(e->d_name + name_len - 2, SUFFIX) == 0) {
				sub[strlen(sub) - 2] = '\0';
				cb(sub, ctx);
			}
		}
		dir[dir_len] = '\0';
		sub[sub_len] = '\0';
	}
	closedir(d);
	return ret;
}

int tedge_port_kv_foreach(const char *prefix,
			  void (*cb)(const char *subkey, void *ctx), void *ctx)
{
	char dir[PATH_MAX];
	char sub[PATH_MAX] = "";
	int ret = key_path(dir, sizeof(dir), prefix, "");

	return (ret == 0) ? walk(dir, sizeof(dir), sub, sizeof(sub), cb, ctx)
			  : ret;
}
