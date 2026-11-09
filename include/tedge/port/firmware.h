/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief Firmware update slots: the port interface.
 *
 * The core downloads a new image and decides when to install and confirm
 * it. The port owns the slots and the bootloader, which follow the
 * MCUboot model: the new image is written to a second slot, booted once
 * on trial, and kept only once it confirms itself; a reset before that
 * brings the old image back.
 */

#ifndef TEDGE_PORT_FIRMWARE_H_
#define TEDGE_PORT_FIRMWARE_H_

#include <stdbool.h>
#include <stddef.h>

/** Ready the update slot for a new image. @return 0, or a negative errno. */
int tedge_port_fw_begin(void);

/** Write the next @p len bytes of the image. @return 0, or a negative
 *  errno. */
int tedge_port_fw_write(const void *data, size_t len);

/** The whole image is written: flush what is buffered. @return 0, or a
 *  negative errno. */
int tedge_port_fw_finish(void);

/** Boot the new image once, on trial, at the next reset. @return 0, or a
 *  negative errno (the image is not valid, for one). */
int tedge_port_fw_request_trial(void);

/** True when the running image is confirmed: not on trial. */
bool tedge_port_fw_is_confirmed(void);

/** Keep the running image: it is no longer on trial. @return 0, or a
 *  negative errno. */
int tedge_port_fw_confirm(void);

/**
 * The running image's version, as its header states it ("1.2.3").
 *
 * @return 0, or -ENOENT when the image carries none.
 */
int tedge_port_fw_running_version(char *buf, size_t len);

#endif /* TEDGE_PORT_FIRMWARE_H_ */
