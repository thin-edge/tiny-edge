/* SPDX-License-Identifier: Apache-2.0
 *
 * The POSIX side of <tedge/port/crypto.h>: where the device key is kept, a
 * PEM file in the state directory readable by the owner only, as
 * thin-edge.io keeps its keys on Linux (device-key.pem); and the system's
 * random numbers. Generating the key and signing the CSR are shared with
 * other ports (ports/common/src/tedge_mbedtls_key.c).
 */

#include <tedge/port/crypto.h>

#include "tedge_common.h"
#include "tedge_posix_internal.h"

#include <errno.h>
#include <limits.h>
#include <sys/random.h>
#include <unistd.h> /* getentropy() on glibc */

/* PSA's key slots and random generator are shared between the client's
 * threads, which are pthreads. */
#if !defined(MBEDTLS_THREADING_PTHREAD)
#error "tiny-edge needs mbedTLS built with MBEDTLS_THREADING_PTHREAD"
#endif

#define KEY_FILE "device-key.pem"

/* The path is made on each call: the state directory is the
 * application's to set (tedge_posix_configure()), and tests change it. */
static char key_file[PATH_MAX];

const char *tedge_common_key_where(void)
{
	if (tedge_posix_path(key_file, sizeof(key_file), KEY_FILE) != 0) {
		return KEY_FILE;
	}
	return key_file;
}

int tedge_common_key_load(char *pem, size_t cap)
{
	char path[PATH_MAX];
	ssize_t n;
	int ret = tedge_posix_path(path, sizeof(path), KEY_FILE);

	if (ret != 0) {
		return ret;
	}
	n = tedge_posix_read_file(path, pem, cap);
	return (n > INT_MAX) ? -EFBIG : (int)n;
}

int tedge_common_key_store(const char *pem, size_t len)
{
	char path[PATH_MAX];
	int ret = tedge_posix_path(path, sizeof(path), KEY_FILE);

	return (ret == 0) ? tedge_posix_write_file(path, pem, len, 0600) : ret;
}

int tedge_port_random(void *buf, size_t len)
{
	uint8_t *p = buf;

	/* getentropy() gives at most 256 bytes a call. */
	while (len > 0) {
		size_t n = (len > 256) ? 256 : len;

		if (getentropy(p, n) != 0) {
			return -errno;
		}
		p += n;
		len -= n;
	}
	return 0;
}
