/* SPDX-License-Identifier: Apache-2.0 */

/* Shared between the POSIX port's own sources; not part of the port
 * interface. */

#ifndef TEDGE_POSIX_INTERNAL_H_
#define TEDGE_POSIX_INTERNAL_H_

#include <tedge/posix.h>

#include <net/if.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/** The configuration tedge_posix_configure() was given. */
const struct tedge_posix_config *tedge_posix_config(void);

/** @p rel under the state directory, into @p buf.
 *  @return 0, or -ENAMETOOLONG. */
int tedge_posix_path(char *buf, size_t len, const char *rel);

/** Create @p path and every missing directory above it, with @p mode.
 *  @return 0, or a negative errno. */
int tedge_posix_mkdirs(const char *path, mode_t mode);

/** Replace @p path with @p len bytes of @p data, all or nothing: written to
 *  a temporary file, flushed, then renamed over it.
 *  @return 0, or a negative errno. */
int tedge_posix_write_file(const char *path, const void *data, size_t len,
			   mode_t mode);

/** Read up to @p cap bytes of @p path.
 *  @return The file's length (which may be more than @p cap), or a
 *          negative errno: -ENOENT when there is no such file. */
ssize_t tedge_posix_read_file(const char *path, void *buf, size_t cap);

/** The name of the interface the client uses (see
 *  tedge_posix_config::interface). @return 0, or -ENODEV when none is up. */
int tedge_posix_interface(char name[IF_NAMESIZE]);

/** Close @p fd in the child of an exec: every descriptor the port opens is
 *  marked, so a restart does not leak them into the new process. */
void tedge_posix_cloexec(int fd);

#endif /* TEDGE_POSIX_INTERNAL_H_ */
