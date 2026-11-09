/* SPDX-License-Identifier: Apache-2.0 */

/* The log macros of <tedge/port/log.h>, as printf, which is what a port
 * without a logging subsystem would do. */

#ifndef TEDGE_HOST_COMPILE_LOG_H_
#define TEDGE_HOST_COMPILE_LOG_H_

#include <stdio.h>

#define TEDGE_LOG_REGISTER()
#define TEDGE_LOG_DECLARE()

#define TEDGE_LOG_ERR(fmt, ...) printf("E: " fmt "\n", ##__VA_ARGS__)
#define TEDGE_LOG_WRN(fmt, ...) printf("W: " fmt "\n", ##__VA_ARGS__)
#define TEDGE_LOG_INF(fmt, ...) printf("I: " fmt "\n", ##__VA_ARGS__)
#define TEDGE_LOG_DBG(fmt, ...) printf("D: " fmt "\n", ##__VA_ARGS__)

#endif /* TEDGE_HOST_COMPILE_LOG_H_ */
